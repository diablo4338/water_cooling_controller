#include "metrics_ble.h"

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "access.h"
#include "state.h"

#include "device_status.h"
#include "fan_control.h"
#include "ina226.h"
#include "metrics.h"

static const char *METRICS_TAG = "metrics";

#define METRICS_TASK_PERIOD_MS 250

uint16_t g_temp_attr_handles[METRICS_TEMP_CHANNELS] = {0};
uint16_t g_fan_attr_handles[METRICS_FAN_CHANNELS] = {0};
uint16_t g_voltage_attr_handle = 0;
uint16_t g_current_attr_handle = 0;
uint16_t g_fan_percent_attr_handle = 0;

static bool g_temp_notify_enabled[METRICS_TEMP_CHANNELS] = {false};
static bool g_fan_notify_enabled[METRICS_FAN_CHANNELS] = {false};
static bool g_voltage_notify_enabled = false;
static bool g_current_notify_enabled = false;
static bool g_fan_percent_notify_enabled = false;
static float g_last_fan_percent = NAN;

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
    g_fan_percent_attr_handle = 0;
    g_voltage_notify_enabled = false;
    g_current_notify_enabled = false;
    g_fan_percent_notify_enabled = false;
    g_last_fan_percent = NAN;
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
    if (attr_handle == g_fan_percent_attr_handle) {
        g_fan_percent_notify_enabled = enabled;
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
    g_fan_percent_notify_enabled = false;
    g_last_fan_percent = NAN;
}

static bool metrics_can_notify(uint16_t conn_handle) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    if (!can_access_data(conn_handle)) return false;
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
    float last_voltage = NAN;
    float last_current = NAN;
    bool last_power_valid = false;

    while (1) {
        TickType_t start = xTaskGetTickCount();

        uint16_t changed = metrics_sample_all();
        ina226_snapshot_t power = ina226_get_snapshot();
        bool voltage_changed = power.sample_valid != last_power_valid ||
                               (power.sample_valid && fabsf(last_voltage - power.voltage_v) >= 0.01f);
        bool current_changed = power.sample_valid != last_power_valid ||
                               (power.sample_valid && fabsf(last_current - power.current_ma) >= 0.1f);
        last_power_valid = power.sample_valid;
        float fan_percent = fan_control_get_output_percent();
        bool fan_percent_changed = !isfinite(g_last_fan_percent) || fabsf(g_last_fan_percent - fan_percent) >= 0.5f;
        if (fan_percent_changed) {
            g_last_fan_percent = fan_percent;
        }
        device_status_set_error_flag(DEVICE_ERROR_INA_OFFLINE, power.state == INA226_OFFLINE);
        if (changed != 0 || fan_percent_changed || voltage_changed || current_changed) {
            uint16_t conn = fsm_get_auth_conn_handle();
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
            if (voltage_changed) {
                last_voltage = power.sample_valid ? power.voltage_v : NAN;
                metrics_notify_float(conn, g_voltage_attr_handle, g_voltage_notify_enabled, last_voltage, "voltage");
            }
            if (current_changed) {
                last_current = power.sample_valid ? power.current_ma : NAN;
                metrics_notify_float(conn, g_current_attr_handle, g_current_notify_enabled, last_current, "current");
            }
            if (fan_percent_changed) {
                metrics_notify_float(conn, g_fan_percent_attr_handle, g_fan_percent_notify_enabled, fan_percent, "fan_percent");
            }
        }

        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t delay = pdMS_TO_TICKS(METRICS_TASK_PERIOD_MS);
        if (elapsed < delay) {
            vTaskDelay(delay - elapsed);
        }
    }
}
