#include "shared_i2c_bus.h"

#include "esp_log.h"

static const char *TAG = "shared-i2c";
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t shared_i2c_bus_init(void) {
    if (s_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = SHARED_I2C_PORT,
        .sda_io_num = SHARED_I2C_SDA_GPIO,
        .scl_io_num = SHARED_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        err = i2c_master_get_bus_handle(SHARED_I2C_PORT, &s_bus);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shared i2c init err=%s", esp_err_to_name(err));
        s_bus = NULL;
    }
    return err;
}

i2c_master_bus_handle_t shared_i2c_bus_get_handle(void) {
    return s_bus;
}
