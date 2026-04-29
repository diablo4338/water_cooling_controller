#include "metrics_ble.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "access.h"
#include "conn_guard.h"
#include "state.h"

#include "device_status.h"
#include "ina226.h"
#include "metrics.h"

static const char *METRICS_TAG = "metrics";

#define METRICS_TASK_PERIOD_MS 250

uint16_t g_temp_attr_handles[METRICS_TEMP_CHANNELS] = {0};
uint16_t g_fan_attr_handles[METRICS_FAN_CHANNELS] = {0};
uint16_t g_voltage_attr_handle = 0;
uint16_t g_current_attr_handle = 0;

static bool g_temp_notify_enabled[METRICS_TEMP_CHANNELS] = {false};
static bool g_fan_notify_enabled[METRICS_FAN_CHANNELS] = {false};
static bool g_voltage_notify_enabled = false;
static bool g_current_notify_enabled = false;

void metrics_ble_init(void) {
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        g_temp_attr_handles[i] = 0;
        g_temp_notify_enabled[i] = false;
    }
    for (int i = 0; i < METRICS_FAN_CHANNELS; i++) {
        g_fan_attr_handles[i] = 0;
        g_fan_notify_enabled[i] = false;
    }
    g_voltage_attr_handle = 0;
    g_current_attr_handle = 0;
    g_voltage_notify_enabled = false;
    g_current_notify_enabled = false;
}

void metrics_set_notify(uint16_t attr_handle, bool enabled) {
    if (attr_handle == 0) return;
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        if (attr_handle == g_temp_attr_handles[i]) {
            g_temp_notify_enabled[i] = enabled;
            return;
        }
    }
    for (int i = 0; i < METRICS_FAN_CHANNELS; i++) {
        if (attr_handle == g_fan_attr_handles[i]) {
            g_fan_notify_enabled[i] = enabled;
            return;
        }
    }
    if (attr_handle == g_voltage_attr_handle) {
        g_voltage_notify_enabled = enabled;
        return;
    }
    if (attr_handle == g_current_attr_handle) {
        g_current_notify_enabled = enabled;
        return;
    }
}

void metrics_reset_notify(void) {
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        g_temp_notify_enabled[i] = false;
    }
    for (int i = 0; i < METRICS_FAN_CHANNELS; i++) {
        g_fan_notify_enabled[i] = false;
    }
    g_voltage_notify_enabled = false;
    g_current_notify_enabled = false;
}

static bool metrics_can_notify(uint16_t conn_handle) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    if (!auth_conn_check(conn_handle)) return false;
    if (!can_access_data()) return false;
    return true;
}

static void metrics_notify_channel(uint16_t conn_handle, uint8_t channel) {
    if (channel >= METRICS_TEMP_CHANNELS) return;
    if (!g_temp_notify_enabled[channel]) return;
    if (!metrics_can_notify(conn_handle)) return;
    if (g_temp_attr_handles[channel] == 0) return;

    float temp = metrics_get_temp(channel);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&temp, sizeof(temp));
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, g_temp_attr_handles[channel], om);
    if (rc != 0) {
        ESP_LOGW(METRICS_TAG, "notify temp[%u] rc=%d", (unsigned)channel, rc);
    }
}

static void metrics_notify_fan(uint16_t conn_handle, uint8_t channel) {
    if (channel >= METRICS_FAN_CHANNELS) return;
    if (!g_fan_notify_enabled[channel]) return;
    if (!metrics_can_notify(conn_handle)) return;
    if (g_fan_attr_handles[channel] == 0) return;

    float rpm = metrics_get_fan_speed_rpm_channel(channel);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&rpm, sizeof(rpm));
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, g_fan_attr_handles[channel], om);
    if (rc != 0) {
        ESP_LOGW(METRICS_TAG, "notify fan[%u] rc=%d", (unsigned)channel, rc);
    }
}

static void metrics_notify_float(uint16_t conn_handle, uint16_t attr_handle, bool enabled, float value, const char *name) {
    if (!enabled) return;
    if (!metrics_can_notify(conn_handle)) return;
    if (attr_handle == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(value));
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(METRICS_TAG, "notify %s rc=%d", name, rc);
    }
}

void metrics_task(void *param) {
    (void)param;

    while (1) {
        TickType_t start = xTaskGetTickCount();

        uint16_t changed = metrics_sample_all();
        device_status_set_error_flag(DEVICE_ERROR_ADC_OFFLINE, metrics_has_error());
        device_status_set_error_flag(DEVICE_ERROR_OVERCURRENT, ina226_overcurrent_active());
        if (changed != 0) {
            uint16_t conn = fsm_get_conn_handle();
            for (uint8_t ch = 0; ch < METRICS_TEMP_CHANNELS; ch++) {
                if (changed & (uint16_t)(1U << ch)) {
                    metrics_notify_channel(conn, ch);
                }
            }
            for (uint8_t ch = 0; ch < METRICS_FAN_CHANNELS; ch++) {
                if (changed & (uint16_t)METRICS_FAN_CHANGED_BIT(ch)) {
                    metrics_notify_fan(conn, ch);
                }
            }
            if (changed & METRICS_VOLTAGE_CHANGED_BIT) {
                metrics_notify_float(conn, g_voltage_attr_handle, g_voltage_notify_enabled, metrics_get_voltage_v(), "voltage");
            }
            if (changed & METRICS_CURRENT_CHANGED_BIT) {
                metrics_notify_float(conn, g_current_attr_handle, g_current_notify_enabled, metrics_get_current_ma(), "current");
            }
        }

        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t delay = pdMS_TO_TICKS(METRICS_TASK_PERIOD_MS);
        if (elapsed < delay) {
            vTaskDelay(delay - elapsed);
        }
    }
}
