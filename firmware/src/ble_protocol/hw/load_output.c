#include "load_output.h"

#include "driver/gpio.h"
#include "esp_err.h"

#define LOAD_ENABLE_GPIO GPIO_NUM_3
#define LOAD_DC_GPIO GPIO_NUM_10

esp_err_t load_output_set(bool enabled) {
    esp_err_t err;
    /* Disable the supply before switching off the inverted DC control. */
    if (!enabled) {
        err = gpio_set_level(LOAD_ENABLE_GPIO, 0);
        if (err != ESP_OK) return err;
    }
    err = gpio_set_level(LOAD_DC_GPIO, enabled ? 0 : 1);
    if (err != ESP_OK) return err;
    if (enabled) return gpio_set_level(LOAD_ENABLE_GPIO, 1);
    return ESP_OK;
}

esp_err_t load_output_verify(bool expected_enabled, bool *matches) {
    if (!matches) return ESP_ERR_INVALID_ARG;

    int enable_level = gpio_get_level(LOAD_ENABLE_GPIO);
    int dc_level = gpio_get_level(LOAD_DC_GPIO);
    if ((enable_level != 0 && enable_level != 1) ||
        (dc_level != 0 && dc_level != 1)) {
        return ESP_FAIL;
    }

    *matches = enable_level == (expected_enabled ? 1 : 0) &&
               dc_level == (expected_enabled ? 0 : 1);
    return ESP_OK;
}

esp_err_t load_output_restore(bool enabled) {
    /* Preload output latches before enabling output drivers or releasing hold. */
    esp_err_t err = load_output_set(enabled);
    if (err != ESP_OK) return err;
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << LOAD_ENABLE_GPIO) | (1ULL << LOAD_DC_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) return err;
    err = load_output_set(enabled);
    if (err != ESP_OK) return err;
    /* Per-pin hold bridges wakeup until the restored configuration is ready. */
    gpio_deep_sleep_hold_dis();
    err = gpio_hold_dis(LOAD_DC_GPIO);
    if (err != ESP_OK) return err;
    return gpio_hold_dis(LOAD_ENABLE_GPIO);
}

esp_err_t load_output_hold(void) {
    esp_err_t err = gpio_hold_en(LOAD_ENABLE_GPIO);
    if (err != ESP_OK) return err;
    err = gpio_hold_en(LOAD_DC_GPIO);
    if (err != ESP_OK) return err;
    gpio_deep_sleep_hold_en();
    return ESP_OK;
}
