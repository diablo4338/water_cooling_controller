#include "ina226.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "shared_i2c_bus.h"

static const char *INA_TAG = "ina226";

#define INA226_CONFIG_VALUE      0x4D37
#define INA226_ALERT_LIMIT_VALUE ((uint16_t)((INA226_ALERT_THRESHOLD_MA * 1000U) / INA226_CURRENT_LSB_UA))
#define INA226_MASK_SOL_BIT      (1U << 15)
#define INA226_MASK_LEN_BIT      (1U << 0)
#define INA226_MASK_ENABLE_VALUE (INA226_MASK_SOL_BIT | INA226_MASK_LEN_BIT)
#define INA226_I2C_TIMEOUT_MS    100

static bool s_i2c_inited = false;
static bool s_ina_error = false;
static bool s_started = false;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;

static esp_err_t ina226_write_reg(uint8_t reg, uint16_t value) {
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), INA226_I2C_TIMEOUT_MS);
}

static esp_err_t ina226_read_reg(uint8_t reg, uint16_t *out) {
    if (!s_i2c_dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(
        s_i2c_dev, &reg, 1, data, sizeof(data), INA226_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *out = (uint16_t)((data[0] << 8) | data[1]);
    return ESP_OK;
}

static void ina226_recover(const char *stage, esp_err_t err) {
    ESP_LOGW(INA_TAG, "%s err=%s", stage, esp_err_to_name(err));
    s_ina_error = true;
    if (s_i2c_bus) {
        esp_err_t reset_err = i2c_master_bus_reset(s_i2c_bus);
        if (reset_err != ESP_OK) {
            ESP_LOGW(INA_TAG, "i2c bus reset err=%s", esp_err_to_name(reset_err));
        }
    }
}

static bool ina226_verify_reg(uint8_t reg, uint16_t expected, const char *name) {
    uint16_t actual = 0;
    esp_err_t err = ina226_read_reg(reg, &actual);
    if (err != ESP_OK) {
        ina226_recover(name, err);
        return false;
    }
    if (actual != expected) {
        ESP_LOGW(INA_TAG, "%s mismatch wrote=0x%04X read=0x%04X", name, expected, actual);
    } else {
        ESP_LOGI(INA_TAG, "%s=0x%04X", name, actual);
    }
    return true;
}

static bool ina226_configure_device(void) {
    esp_err_t err = ina226_write_reg(REG_CONFIG, INA226_CONFIG_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write config", err);
        return false;
    }
    if (!ina226_verify_reg(REG_CONFIG, INA226_CONFIG_VALUE, "config")) return false;

    err = ina226_write_reg(REG_CALIBRATION, CAL_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write calibration", err);
        return false;
    }
    if (!ina226_verify_reg(REG_CALIBRATION, CAL_VALUE, "calibration")) return false;

    err = ina226_write_reg(REG_ALERT_LIMIT, INA226_ALERT_LIMIT_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write alert limit", err);
        return false;
    }
    if (!ina226_verify_reg(REG_ALERT_LIMIT, INA226_ALERT_LIMIT_VALUE, "alert_limit")) return false;

    err = ina226_write_reg(REG_MASK_ENABLE, INA226_MASK_ENABLE_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write mask/enable", err);
        return false;
    }
    if (!ina226_verify_reg(REG_MASK_ENABLE, INA226_MASK_ENABLE_VALUE, "mask_enable")) return false;

    return true;
}

bool ina226_init(void) {
    if (s_started && !s_ina_error && s_i2c_dev != NULL) return true;

    if (s_started) {
        ESP_LOGW(INA_TAG, "reinitializing after INA/I2C error");
    }

    s_i2c_bus = shared_i2c_bus_get_handle();
    if (s_i2c_bus == NULL) {
        ESP_LOGW(INA_TAG, "shared i2c bus is not initialized");
        s_ina_error = true;
        return false;
    }

    if (!s_i2c_dev) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = INA226_ADDR,
            .scl_speed_hz = INA226_I2C_FREQ_HZ,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = 0,
            },
        };
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
        if (err != ESP_OK) {
            ESP_LOGW(INA_TAG, "i2c add device err=%s", esp_err_to_name(err));
            s_ina_error = true;
            return false;
        }
        s_i2c_inited = true;
    }

    if (!ina226_configure_device()) return false;

    s_started = true;
    s_ina_error = false;
    ESP_LOGI(INA_TAG, "initialized addr=0x%02X", INA226_ADDR);
    return true;
}

bool ina226_has_error(void) {
    return s_ina_error || (s_i2c_inited && s_i2c_dev == NULL);
}

bool ina226_get_sample(ina226_sample_t *out) {
    if (!out) return false;
    if ((!s_started || s_ina_error) && !ina226_init()) return false;
    if (!s_i2c_dev) return false;

    uint16_t bus_raw = 0;
    uint16_t current_raw_u = 0;

    esp_err_t err = ina226_read_reg(REG_BUS_VOLTAGE, &bus_raw);
    if (err != ESP_OK) {
        ina226_recover("read bus voltage", err);
        return false;
    }

    err = ina226_read_reg(REG_CURRENT, &current_raw_u);
    if (err != ESP_OK) {
        ina226_recover("read current", err);
        return false;
    }

    *out = (ina226_sample_t) {
        .bus_raw = bus_raw,
        .current_raw = (int16_t)current_raw_u,
        .voltage_v = ina226_bus_raw_to_v(bus_raw),
        .current_ma = ina226_current_raw_to_ma((int16_t)current_raw_u),
        .valid = true,
    };
    s_ina_error = false;
    return true;
}

bool ina226_read_mask_enable(uint16_t *out) {
    if (!out) return false;
    if ((!s_started || s_ina_error) && !ina226_init()) return false;
    if (!s_i2c_dev) return false;

    esp_err_t err = ina226_read_reg(REG_MASK_ENABLE, out);
    if (err != ESP_OK) {
        ina226_recover("read mask/enable", err);
        return false;
    }

    s_ina_error = false;
    return true;
}

float ina226_bus_raw_to_v(uint16_t raw) {
    return (float)raw * 1.25e-3f;
}

float ina226_current_raw_to_ma(int16_t raw) {
    return (float)raw * 0.05f;
}
