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
#include "ina226.h"

#define INA226_ALERT_GPIO 8
#define OVERCURRENT_POLL_MS 500
#define OVERCURRENT_LATCH_US (2000 * 1000LL)
#define OVERCURRENT_TASK_STACK 2048
#define OVERCURRENT_TASK_PRIORITY 4

static const char *TAG = "overcurrent";
static bool s_started = false;
static volatile bool s_alert_latched = false;

#define INA226_MASK_AFF_BIT  (1U << 4)
#define INA226_MASK_CVRF_BIT (1U << 3)
#define INA226_MASK_OVF_BIT  (1U << 2)
#define INA226_MASK_APOL_BIT (1U << 1)
#define INA226_MASK_LEN_BIT  (1U << 0)

bool overcurrent_monitor_alert_active(void) {
    if (!s_started) return false;
    return gpio_get_level(INA226_ALERT_GPIO) == 0;
}

bool overcurrent_monitor_latched_active(void) {
    return s_alert_latched;
}

bool overcurrent_monitor_read_alert_status(uint16_t *mask_enable) {
    return ina226_read_mask_enable(mask_enable);
}

void overcurrent_monitor_clear_latched(void) {
    s_alert_latched = false;
    device_status_set_error_flag(DEVICE_ERROR_OVERCURRENT, false);
}

static void overcurrent_log_mask_enable(const char *prefix, uint16_t mask_enable) {
    ESP_LOGW(TAG, "%s mask=0x%04X aff=%d cvrf=%d ovf=%d apol=%d len=%d",
             prefix,
             mask_enable,
             (mask_enable & INA226_MASK_AFF_BIT) ? 1 : 0,
             (mask_enable & INA226_MASK_CVRF_BIT) ? 1 : 0,
             (mask_enable & INA226_MASK_OVF_BIT) ? 1 : 0,
             (mask_enable & INA226_MASK_APOL_BIT) ? 1 : 0,
             (mask_enable & INA226_MASK_LEN_BIT) ? 1 : 0);
}

static void overcurrent_monitor_task(void *arg) {
    (void)arg;

    int64_t confirm_deadline_us = 0;
    bool confirm_pending = false;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        bool alert_active = overcurrent_monitor_alert_active();

        if (!s_alert_latched && alert_active && !confirm_pending) {
            uint16_t mask_enable = 0;
            ESP_LOGW(TAG, "raw alert active: read mask/enable and start confirm wait");
            if (overcurrent_monitor_read_alert_status(&mask_enable)) {
                overcurrent_log_mask_enable("raw alert sample", mask_enable);
            } else {
                ESP_LOGW(TAG, "raw alert sample: mask/enable read failed");
            }
            confirm_pending = true;
            confirm_deadline_us = now_us + OVERCURRENT_LATCH_US;
        }

        if (!s_alert_latched && confirm_pending && now_us >= confirm_deadline_us) {
            if (overcurrent_monitor_alert_active()) {
                uint16_t mask_enable = 0;
                if (overcurrent_monitor_read_alert_status(&mask_enable)) {
                    overcurrent_log_mask_enable("confirm alert sample", mask_enable);
                } else {
                    ESP_LOGW(TAG, "confirm alert sample: mask/enable read failed");
                }
                s_alert_latched = true;
                ESP_LOGW(TAG, "overcurrent confirmed after %lld ms: start recovery",
                         OVERCURRENT_LATCH_US / 1000LL);
                device_status_set_error_flag(DEVICE_ERROR_OVERCURRENT, true);
            } else {
                ESP_LOGI(TAG, "confirm wait expired: alert not present, skip recovery");
            }
            confirm_pending = false;
            confirm_deadline_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(OVERCURRENT_POLL_MS));
    }
}

void overcurrent_monitor_init(void) {
    if (s_started) return;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << INA226_ALERT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
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
    s_alert_latched = false;
}
