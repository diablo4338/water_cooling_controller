#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_status.h"
#include "fan_control.h"
#include "state.h"

#define STATUS_LED_GPIO 1
#define STATUS_LED_ON_LEVEL 1
#define STATUS_LED_OFF_LEVEL 0
#define STATUS_LED_POLL_MS 50
#define STATUS_LED_SLOW_BLINK_MS 2000
#define STATUS_LED_FAST_BLINK_MS 500

typedef enum {
    STATUS_LED_MODE_OFF = 0,
    STATUS_LED_MODE_ON,
    STATUS_LED_MODE_BLINK_SLOW,
    STATUS_LED_MODE_BLINK_FAST,
} status_led_mode_t;

static void status_led_set_level(int level) {
    gpio_set_level(STATUS_LED_GPIO, level);
}

static status_led_mode_t status_led_resolve_mode(void) {
    if (fan_control_is_overheat()) {
        return STATUS_LED_MODE_BLINK_FAST;
    }
    if (device_status_is_error()) {
        return STATUS_LED_MODE_BLINK_SLOW;
    }
    if (fsm_is_pairing()) {
        return STATUS_LED_MODE_ON;
    }
    return STATUS_LED_MODE_OFF;
}

void status_led_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    status_led_set_level(STATUS_LED_OFF_LEVEL);
}

void status_led_task(void *param) {
    (void)param;

    TickType_t last_toggle_tick = xTaskGetTickCount();
    status_led_mode_t last_mode = STATUS_LED_MODE_OFF;
    bool output_on = false;

    while (1) {
        status_led_mode_t mode = status_led_resolve_mode();
        TickType_t now = xTaskGetTickCount();

        if (mode != last_mode) {
            last_mode = mode;
            last_toggle_tick = now;
            output_on = (mode == STATUS_LED_MODE_BLINK_SLOW || mode == STATUS_LED_MODE_BLINK_FAST);
        }

        switch (mode) {
            case STATUS_LED_MODE_ON:
                output_on = true;
                break;
            case STATUS_LED_MODE_BLINK_SLOW:
                if ((now - last_toggle_tick) >= pdMS_TO_TICKS(STATUS_LED_SLOW_BLINK_MS)) {
                    output_on = !output_on;
                    last_toggle_tick = now;
                }
                break;
            case STATUS_LED_MODE_BLINK_FAST:
                if ((now - last_toggle_tick) >= pdMS_TO_TICKS(STATUS_LED_FAST_BLINK_MS)) {
                    output_on = !output_on;
                    last_toggle_tick = now;
                }
                break;
            case STATUS_LED_MODE_OFF:
            default:
                output_on = false;
                break;
        }

        status_led_set_level(output_on ? STATUS_LED_ON_LEVEL : STATUS_LED_OFF_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_POLL_MS));
    }
}
