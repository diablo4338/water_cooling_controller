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
#include "metrics.h"

static const char *METRICS_TAG = "metrics";

uint16_t g_temp_attr_handles[METRICS_TEMP_CHANNELS] = {0};
uint16_t g_fan_attr_handle = 0;

static bool g_temp_notify_enabled[METRICS_TEMP_CHANNELS] = {false};
static bool g_fan_notify_enabled = false;

void metrics_ble_init(void) {
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        g_temp_attr_handles[i] = 0;
        g_temp_notify_enabled[i] = false;
    }
    g_fan_attr_handle = 0;
    g_fan_notify_enabled = false;
}

void metrics_set_notify(uint16_t attr_handle, bool enabled) {
    if (attr_handle == 0) return;
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        if (attr_handle == g_temp_attr_handles[i]) {
            g_temp_notify_enabled[i] = enabled;
            return;
        }
    }
    if (attr_handle == g_fan_attr_handle) {
        g_fan_notify_enabled = enabled;
    }
}

void metrics_reset_notify(void) {
    for (int i = 0; i < METRICS_TEMP_CHANNELS; i++) {
        g_temp_notify_enabled[i] = false;
    }
    g_fan_notify_enabled = false;
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

static void metrics_notify_fan(uint16_t conn_handle) {
    if (!g_fan_notify_enabled) return;
    if (!metrics_can_notify(conn_handle)) return;
    if (g_fan_attr_handle == 0) return;

    float rpm = metrics_get_fan_speed_rpm();
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&rpm, sizeof(rpm));
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, g_fan_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(METRICS_TAG, "notify fan rc=%d", rc);
    }
}

void metrics_task(void *param) {
    (void)param;

    while (1) {
        TickType_t start = xTaskGetTickCount();

        uint8_t changed = metrics_sample_all();
        device_status_set_error(metrics_has_error() ? DEVICE_ERROR_ADC_OFFLINE : DEVICE_ERROR_NONE);
        if (changed != 0) {
            uint16_t conn = fsm_get_conn_handle();
            for (uint8_t ch = 0; ch < METRICS_TEMP_CHANNELS; ch++) {
                if (changed & (uint8_t)(1U << ch)) {
                    metrics_notify_channel(conn, ch);
                }
            }
            if (changed & METRICS_FAN_CHANGED_BIT) {
                metrics_notify_fan(conn);
            }
        }

        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t delay = pdMS_TO_TICKS(1000);
        if (elapsed < delay) {
            vTaskDelay(delay - elapsed);
        }
    }
}
