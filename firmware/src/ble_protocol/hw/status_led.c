#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_status.h"
#include "pair_mode.h"
#include "state.h"

#define STATUS_LED_GPIO 1
#define STATUS_LED_ON_LEVEL 1
#define STATUS_LED_OFF_LEVEL 0
#define STATUS_LED_POLL_MS 50
#define STATUS_LED_SLOW_BLINK_MS 2000
#define STATUS_LED_FAST_BLINK_MS 120
#define STATUS_LED_FACTORY_BLINK_MS 150
#define STATUS_LED_FACTORY_SUCCESS_TOGGLES 4
#define STATUS_LED_FACTORY_FAILURE_TOGGLES 6

static uint32_t s_factory_blink_request;

static void status_led_set_level(int level) {
    gpio_set_level(STATUS_LED_GPIO, level);
}

status_led_mode_t status_led_resolve_mode_for_state(bool pairing_active, bool overheat_active, bool error_active) {
    if (pairing_active) {
        return STATUS_LED_MODE_ON;
    }
    if (overheat_active) {
        return STATUS_LED_MODE_BLINK_FAST;
    }
    if (error_active) {
        return STATUS_LED_MODE_BLINK_SLOW;
    }
    return STATUS_LED_MODE_OFF;
}

static status_led_mode_t status_led_resolve_mode(void) {
    if (device_status_has_error_flag(DEVICE_ERROR_OUTPUT_ALARM)) {
        return STATUS_LED_MODE_BLINK_FAST;
    }
    return status_led_resolve_mode_for_state(
        pair_mode_is_active(),
        device_status_has_error_flag(DEVICE_ERROR_OVERHEAT),
        device_status_is_error()
    );
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

void status_led_report_factory_reset(bool success) {
    uint32_t toggles = success ? STATUS_LED_FACTORY_SUCCESS_TOGGLES : STATUS_LED_FACTORY_FAILURE_TOGGLES;
    __atomic_store_n(&s_factory_blink_request, toggles, __ATOMIC_RELEASE);
}

void status_led_task(void *param) {
    (void)param;

    TickType_t last_toggle_tick = xTaskGetTickCount();
    status_led_mode_t last_mode = STATUS_LED_MODE_OFF;
    bool output_on = false;
    uint32_t factory_blink_remaining = 0;

    while (1) {
        uint32_t request = __atomic_exchange_n(&s_factory_blink_request, 0, __ATOMIC_ACQ_REL);
        if (request > 0) {
            factory_blink_remaining = request;
            output_on = false;
            last_toggle_tick = xTaskGetTickCount() - pdMS_TO_TICKS(STATUS_LED_FACTORY_BLINK_MS);
        }

        if (factory_blink_remaining > 0) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_toggle_tick) >= pdMS_TO_TICKS(STATUS_LED_FACTORY_BLINK_MS)) {
                output_on = !output_on;
                last_toggle_tick = now;
                factory_blink_remaining--;
            }
            status_led_set_level(output_on ? STATUS_LED_ON_LEVEL : STATUS_LED_OFF_LEVEL);
            if (factory_blink_remaining == 0) {
                output_on = false;
                status_led_set_level(STATUS_LED_OFF_LEVEL);
                last_mode = STATUS_LED_MODE_OFF;
                last_toggle_tick = xTaskGetTickCount();
            }
            vTaskDelay(pdMS_TO_TICKS(STATUS_LED_POLL_MS));
            continue;
        }

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
