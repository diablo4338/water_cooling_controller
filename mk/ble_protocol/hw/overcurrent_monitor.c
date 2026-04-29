#include "overcurrent_monitor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_status.h"

#define INA226_ALERT_GPIO 8
#define OVERCURRENT_POLL_MS 500
#define OVERCURRENT_LATCH_US (2000 * 1000LL)
#define OVERCURRENT_TASK_STACK 2048
#define OVERCURRENT_TASK_PRIORITY 4

static const char *TAG = "overcurrent";
static bool s_started = false;

static void overcurrent_monitor_task(void *arg) {
    (void)arg;

    int64_t low_since_us = 0;
    bool error_active = false;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        bool alert_active = gpio_get_level(INA226_ALERT_GPIO) == 0;

        if (alert_active) {
            if (low_since_us == 0) {
                low_since_us = now_us;
            }
            if (!error_active && (now_us - low_since_us) >= OVERCURRENT_LATCH_US) {
                error_active = true;
                device_status_set_error_flag(DEVICE_ERROR_OVERCURRENT, true);
            }
        } else {
            low_since_us = 0;
            if (error_active) {
                error_active = false;
                device_status_set_error_flag(DEVICE_ERROR_OVERCURRENT, false);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(OVERCURRENT_POLL_MS));
    }
}

void overcurrent_monitor_init(void) {
    if (s_started) return;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << INA226_ALERT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    BaseType_t task_ok = xTaskCreate(
        overcurrent_monitor_task,
        "overcurrent",
        OVERCURRENT_TASK_STACK,
        NULL,
        OVERCURRENT_TASK_PRIORITY,
        NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        abort();
    }

    s_started = true;
}
