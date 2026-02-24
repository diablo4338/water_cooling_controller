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
#include "state.h"
#include "storage.h"

void button_task(void *p) {
    (void)p;

    int prev = 1;
    int64_t press_start = 0;

    while (1) {
        int v = gpio_get_level(PAIR_BTN_GPIO);

        if (prev == 1 && v == 0) {
            press_start = esp_timer_get_time();
        }

        if (prev == 0 && v == 1) {
            int64_t dur_ms = (esp_timer_get_time() - press_start) / 1000;

            if (dur_ms > 3000) {
                if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                    ESP_LOGW(TAG, "Reset pair: disconnecting client (handle=%u)",
                             (unsigned)g_conn_handle);
                    ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
                trust_reset();
            } else {
                if (g_paired) {
                    ESP_LOGW(TAG, "Already paired; pairing mode ignored");
                    prev = v;
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                g_pairing_mode = true;
                rand_bytes(dev_nonce, sizeof(dev_nonce));

                if (ecdh_make_dev_keys() != 0) {
                    ESP_LOGE(TAG, "ECDH keygen failed");
                } else {
                    stop_advertising();
                    start_advertising();
                    ESP_LOGW(TAG, "Pairing mode ON (20s)");
                }

                vTaskDelay(pdMS_TO_TICKS(20000));

                if (g_pairing_mode) {
                    g_pairing_mode = false;
                    stop_advertising();
                    start_advertising();
                    ESP_LOGW(TAG, "Pairing mode OFF (timeout)");
                }
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
