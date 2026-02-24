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
#include "pair_state.h"
#include "state.h"
#include "storage.h"
#include "rgb.h"

void pair_timeout_cb(void *arg) {
    (void)arg;
    state_lock();
    if (!g_pairing_mode) {
        state_unlock();
        return;
    }
    g_pairing_mode = false;
    g_pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    state_unlock();
    rgb_set_pairing(false);
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
    bool press_started_pair = false;

    while (1) {
        int v = gpio_get_level(PAIR_BTN_GPIO);

        if (prev == 1 && v == 0) {
            press_start = esp_timer_get_time();
            long_triggered = false;
            press_started_pair = false;

            bool paired;
            bool has_conn;
            state_lock();
            paired = g_paired;
            has_conn = (g_conn_handle != BLE_HS_CONN_HANDLE_NONE);
            state_unlock();
            if (paired) {
                ESP_LOGW(TAG, "Already paired; pairing mode ignored");
            } else if (has_conn) {
                ESP_LOGW(TAG, "Pairing request ignored: device already connected");
            } else {
                state_lock();
                g_pairing_mode = true;
                g_pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                g_auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                state_unlock();
                rand_bytes(dev_nonce, sizeof(dev_nonce));

                if (ecdh_make_dev_keys() != 0) {
                    ESP_LOGE(TAG, "ECDH keygen failed");
                } else {
                    press_started_pair = true;
                    pair_state_start();
                    esp_timer_stop(g_pair_timer);
                    ESP_ERROR_CHECK(esp_timer_start_once(g_pair_timer, 20 * 1000 * 1000));
                    rgb_set_pairing(true);
                    stop_advertising();
                    start_advertising();
                    ESP_LOGW(TAG, "Pairing mode ON (20s)");
                }
            }
        }

        if (v == 0 && !long_triggered) {
            int64_t dur_ms = (esp_timer_get_time() - press_start) / 1000;
            if (dur_ms > 3000) {
                long_triggered = true;
                bool has_conn;
                state_lock();
                has_conn = (g_conn_handle != BLE_HS_CONN_HANDLE_NONE);
                state_unlock();
                if (has_conn) {
                    ESP_LOGW(TAG, "Reset pair: disconnecting client (handle=%u)",
                             (unsigned)g_conn_handle);
                    int rc = ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
                }
                trust_reset();
                rgb_set_connected(false);
                rgb_set_pairing(false);
                rgb_blink_reset();
                stop_advertising();
                start_advertising();
            }
        }

        if (prev == 0 && v == 1) {
            if (long_triggered && press_started_pair) {
                ESP_LOGW(TAG, "Pairing aborted by reset");
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
