#include "load_output.h"

#include "driver/gpio.h"
#include "esp_err.h"

#define LOAD_ENABLE_GPIO GPIO_NUM_3
#define LOAD_DC_GPIO GPIO_NUM_10
#define LOAD_PWM_GPIO GPIO_NUM_2

void load_output_set(bool enabled) {
    /* Disable the supply before switching off the inverted DC control. */
    if (!enabled) ESP_ERROR_CHECK(gpio_set_level(LOAD_ENABLE_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_level(LOAD_PWM_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_level(LOAD_DC_GPIO, enabled ? 0 : 1));
    if (enabled) ESP_ERROR_CHECK(gpio_set_level(LOAD_ENABLE_GPIO, 1));
}

void load_output_restore(bool enabled) {
    /* Preload output latches before enabling output drivers or releasing hold. */
    load_output_set(enabled);
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << LOAD_ENABLE_GPIO) | (1ULL << LOAD_DC_GPIO) |
                        (1ULL << LOAD_PWM_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    load_output_set(enabled);
    /* Per-pin hold bridges wakeup until the restored configuration is ready. */
    gpio_deep_sleep_hold_dis();
    ESP_ERROR_CHECK(gpio_hold_dis(LOAD_DC_GPIO));
    ESP_ERROR_CHECK(gpio_hold_dis(LOAD_PWM_GPIO));
    ESP_ERROR_CHECK(gpio_hold_dis(LOAD_ENABLE_GPIO));
}

void load_output_hold(void) {
    ESP_ERROR_CHECK(gpio_hold_en(LOAD_ENABLE_GPIO));
    ESP_ERROR_CHECK(gpio_hold_en(LOAD_DC_GPIO));
    ESP_ERROR_CHECK(gpio_hold_en(LOAD_PWM_GPIO));
    gpio_deep_sleep_hold_en();
}
