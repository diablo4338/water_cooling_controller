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
#include "state.h"
#include "storage.h"

void pair_timeout_cb(void *arg) {
    (void)arg;
    if (pair_mode_is_forced()) {
        return;
    }
    if (!fsm_dispatch(FSM_EVT_PAIR_TIMEOUT, BLE_HS_CONN_HANDLE_NONE)) {
        return;
    }
    pair_state_full_reset();
    stop_advertising();
    start_advertising();
    ESP_LOGW(TAG, "Pairing mode OFF (timeout)");
}

void button_task(void *p) {
    (void)p;

    int prev = 1;
    int64_t press_start = 0;
    bool long_triggered = false;

    while (1) {
        int v = gpio_get_level(PAIR_BTN_GPIO);

        if (prev == 1 && v == 0) {
            press_start = esp_timer_get_time();
            long_triggered = false;
        }

        if (v == 0 && !long_triggered) {
            int64_t dur_ms = (esp_timer_get_time() - press_start) / 1000;
            if (dur_ms > 3000) {
                long_triggered = true;
                uint16_t conn_handle = fsm_get_conn_handle();
                bool has_conn = (conn_handle != BLE_HS_CONN_HANDLE_NONE);
                if (has_conn) {
                    ESP_LOGW(TAG, "Reset pair: disconnecting client (handle=%u)",
                             (unsigned)conn_handle);
                    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
                }
                trust_reset();
                pair_mode_force_on();
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
