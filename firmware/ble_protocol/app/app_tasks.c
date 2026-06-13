#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_gap.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "crypto.h"
#include "ecdh.h"
#include "gap.h"
#include "pair_mode.h"
#include "pair_state.h"
#include "params.h"
#include "status_led.h"
#include "state.h"
#include "storage.h"

void pair_timeout_cb(void *arg) {
    (void)arg;
    if (!pair_mode_is_active()) {
        return;
    }
    pair_mode_deactivate();

    uint16_t conn_handle = fsm_get_conn_handle();
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE && !fsm_is_authed()) {
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
        return;
    }

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        stop_advertising();
        start_advertising();
    }

    ESP_LOGW(TAG, "Pairing mode OFF (timeout)");
}

void button_task(void *p) {
    (void)p;

    int prev = 1;
    int64_t press_start = 0;
    bool pair_triggered = false;
    bool factory_triggered = false;

    while (1) {
        int v = gpio_get_level(PAIR_BTN_GPIO);

        if (prev == 1 && v == 0) {
            press_start = esp_timer_get_time();
            pair_triggered = false;
            factory_triggered = false;
        }

        if (v == 0 && !factory_triggered) {
            int64_t dur_ms = (esp_timer_get_time() - press_start) / 1000;
            if (dur_ms > 10000) {
                factory_triggered = true;
                uint16_t conn_handle = fsm_get_conn_handle();
                bool has_conn = (conn_handle != BLE_HS_CONN_HANDLE_NONE);
                pair_mode_deactivate();
                trust_reset();
                params_factory_reset();
                status_led_blink_twice();
                ESP_LOGW(TAG, "Factory reset completed");
                if (has_conn) {
                    ESP_LOGW(TAG, "Factory reset: disconnecting client (handle=%u)",
                             (unsigned)conn_handle);
                    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
                } else {
                    stop_advertising();
                    start_advertising();
                }
            } else if (dur_ms > 3000 && !pair_triggered) {
                pair_triggered = true;
                uint16_t conn_handle = fsm_get_conn_handle();
                bool has_conn = (conn_handle != BLE_HS_CONN_HANDLE_NONE);
                if (has_conn) {
                    ESP_LOGW(TAG, "Reset pair: disconnecting client (handle=%u)",
                             (unsigned)conn_handle);
                    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
                }
                pair_mode_activate();
            }
        }

        prev = v;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
