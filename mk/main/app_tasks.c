#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

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

            if (dur_ms > 5000) {
                trust_reset();
            } else {
                g_pairing_mode = true;
                rand_bytes(dev_nonce, sizeof(dev_nonce));

                if (ecdh_make_dev_keys() != 0) {
                    ESP_LOGE(TAG, "ECDH keygen failed");
                } else {
                    stop_advertising();
                    start_advertising();
                    ESP_LOGW(TAG, "Pairing mode ON (60s)");
                }

                vTaskDelay(pdMS_TO_TICKS(60000));

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
