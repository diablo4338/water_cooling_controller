#include "ads1115.h"

#include <math.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *ADS_TAG = "ads1115";
static bool s_i2c_inited = false;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static i2c_device_config_t s_dev_cfg = {0};

#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG     0x01

#define ADS1115_CFG_OS_SINGLE   (1U << 15)
#define ADS1115_CFG_MUX_SHIFT   12
#define ADS1115_CFG_PGA_SHIFT   9
#define ADS1115_CFG_MODE_SINGLE (1U << 8)
#define ADS1115_CFG_DR_SHIFT    5
#define ADS1115_CFG_COMP_QUE    0x0003

#define ADS1115_I2C_TIMEOUT_MS 100
#define ADS1115_RETRY_COUNT    3

static uint16_t ads1115_build_config(uint8_t channel) {
    uint16_t mux = (uint16_t)(0x4 + (channel & 0x03)) << ADS1115_CFG_MUX_SHIFT;
    uint16_t pga = (uint16_t)0x1 << ADS1115_CFG_PGA_SHIFT;  // ±4.096V
    uint16_t dr = (uint16_t)0x4 << ADS1115_CFG_DR_SHIFT;    // 128 SPS
    return (uint16_t)(ADS1115_CFG_OS_SINGLE | mux | pga | ADS1115_CFG_MODE_SINGLE | dr | ADS1115_CFG_COMP_QUE);
}

static void ads1115_recover(const char *stage, esp_err_t err) {
    ESP_LOGW(ADS_TAG, "%s err=%s", stage, esp_err_to_name(err));
    if (!s_i2c_bus) return;

    esp_err_t reset_err = i2c_master_bus_reset(s_i2c_bus);
    if (reset_err != ESP_OK) {
        ESP_LOGW(ADS_TAG, "i2c bus reset err=%s", esp_err_to_name(reset_err));
    }

    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }

    if (s_dev_cfg.device_address != 0) {
        esp_err_t add_err = i2c_master_bus_add_device(s_i2c_bus, &s_dev_cfg, &s_i2c_dev);
        if (add_err != ESP_OK) {
            ESP_LOGW(ADS_TAG, "i2c re-add device err=%s", esp_err_to_name(add_err));
        }
    }
}

void ads1115_init(void) {
    if (s_i2c_inited) return;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ADS1115_I2C_PORT,
        .sda_io_num = ADS1115_I2C_SDA_GPIO,
        .scl_io_num = ADS1115_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(i2c_master_get_bus_handle(ADS1115_I2C_PORT, &s_i2c_bus));
    } else {
        ESP_ERROR_CHECK(err);
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_I2C_ADDR,
        .scl_speed_hz = ADS1115_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    s_dev_cfg = dev_cfg;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &s_dev_cfg, &s_i2c_dev));
    s_i2c_inited = true;
}

bool ads1115_read_raw(uint8_t channel, int16_t *out_raw) {
    if (!out_raw || channel > 3) return false;
    if (!s_i2c_inited) {
        ads1115_init();
    }
    if (!s_i2c_dev) return false;

    uint16_t cfg = ads1115_build_config(channel);
    uint8_t buf[3];
    buf[0] = ADS1115_REG_CONFIG;
    buf[1] = (uint8_t)(cfg >> 8);
    buf[2] = (uint8_t)(cfg & 0xFF);

    for (int attempt = 0; attempt < ADS1115_RETRY_COUNT; attempt++) {
        esp_err_t err = i2c_master_transmit(
            s_i2c_dev, buf, sizeof(buf), ADS1115_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ads1115_recover("i2c write cfg", err);
            vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(ADS1115_CONV_DELAY_MS));

        uint8_t reg = ADS1115_REG_CONVERSION;
        uint8_t data[2] = {0};
        err = i2c_master_transmit_receive(
            s_i2c_dev, &reg, 1, data, sizeof(data), ADS1115_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ads1115_recover("i2c read conv", err);
            vTaskDelay(pdMS_TO_TICKS(ADS1115_INTER_CH_DELAY_MS));
            continue;
        }

        *out_raw = (int16_t)((data[0] << 8) | data[1]);
        return true;
    }
    return false;
}

float ads1115_raw_to_v(int16_t raw) {
    return ((float)raw * ADS1115_FS_V) / 32768.0f;
}

float ads1115_raw_to_temp(int16_t raw) {
    float volt_p = ads1115_raw_to_v(raw);
    if (volt_p <= 0.000001f) return NAN;

    float res_p = ((3.3f * 4700.0f) / volt_p) - 4700.0f;
    if (res_p <= 0.0f) return NAN;

    float inv_t = (1.0f / (25.0f + 273.15f)) + (logf(res_p / 10000.0f) / 3625.0f);
    float temp_c = (1.0f / inv_t) - 273.15f;
    return roundf(temp_c * 100.0f) / 100.0f;
}
