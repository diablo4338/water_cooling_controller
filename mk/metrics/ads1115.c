#include "ads1115.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *ADS_TAG = "ads1115";
static bool s_i2c_inited = false;

#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG     0x01

#define ADS1115_CFG_OS_SINGLE   (1U << 15)
#define ADS1115_CFG_MUX_SHIFT   12
#define ADS1115_CFG_PGA_SHIFT   9
#define ADS1115_CFG_MODE_SINGLE (1U << 8)
#define ADS1115_CFG_DR_SHIFT    5
#define ADS1115_CFG_COMP_QUE    0x0003

static uint16_t ads1115_build_config(uint8_t channel) {
    uint16_t mux = (uint16_t)(0x4 + (channel & 0x03)) << ADS1115_CFG_MUX_SHIFT;
    uint16_t pga = (uint16_t)0x1 << ADS1115_CFG_PGA_SHIFT;  // ±4.096V
    uint16_t dr = (uint16_t)0x4 << ADS1115_CFG_DR_SHIFT;    // 128 SPS
    return (uint16_t)(ADS1115_CFG_OS_SINGLE | mux | pga | ADS1115_CFG_MODE_SINGLE | dr | ADS1115_CFG_COMP_QUE);
}

void ads1115_init(void) {
    if (s_i2c_inited) return;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = ADS1115_I2C_SDA_GPIO,
        .scl_io_num = ADS1115_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = ADS1115_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(ADS1115_I2C_PORT, &conf));
    esp_err_t err = i2c_driver_install(ADS1115_I2C_PORT, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        s_i2c_inited = true;
        return;
    }
    ESP_ERROR_CHECK(err);
    s_i2c_inited = true;
}

bool ads1115_read_raw(uint8_t channel, int16_t *out_raw) {
    if (!out_raw || channel > 3) return false;

    uint16_t cfg = ads1115_build_config(channel);
    uint8_t buf[3];
    buf[0] = ADS1115_REG_CONFIG;
    buf[1] = (uint8_t)(cfg >> 8);
    buf[2] = (uint8_t)(cfg & 0xFF);

    esp_err_t err = i2c_master_write_to_device(
        ADS1115_I2C_PORT, ADS1115_I2C_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(ADS_TAG, "I2C write cfg err=%d", (int)err);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(ADS1115_CONV_DELAY_MS));

    uint8_t reg = ADS1115_REG_CONVERSION;
    uint8_t data[2] = {0};
    err = i2c_master_write_read_device(
        ADS1115_I2C_PORT, ADS1115_I2C_ADDR, &reg, 1, data, sizeof(data), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(ADS_TAG, "I2C read conv err=%d", (int)err);
        return false;
    }

    *out_raw = (int16_t)((data[0] << 8) | data[1]);
    return true;
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
