#include "ina226.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shared_i2c_bus.h"

static const char *INA_TAG = "ina226";

#define INA226_CONFIG_VALUE      0x4D37
#define INA226_MASK_CVRF_BIT     (1U << 3)
#define INA226_MASK_ENABLE_VALUE 0U
#define INA226_CONFIG_FIELDS     0x0FFFU
#define INA226_CAL_FIELDS        0x7FFFU
#define INA226_MASK_ENABLE_FIELDS 0xFC03U
#define INA226_I2C_TIMEOUT_MS    100
#define INA226_POLL_PERIOD_MS    500

static bool s_ina_error = false;
static bool s_started = false;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static ina226_snapshot_t s_snapshot;
static bool s_monitor_started;

typedef struct {
    int64_t mask_read_started_us;
    int64_t conversion_completed_after_us;
} ina226_timing_t;

static esp_err_t ina226_write_reg(uint8_t reg, uint16_t value) {
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;
    if (!shared_i2c_bus_lock(pdMS_TO_TICKS(INA226_I2C_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t buf[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };
    esp_err_t err = i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), INA226_I2C_TIMEOUT_MS);
    shared_i2c_bus_unlock();
    return err;
}

static esp_err_t ina226_read_reg(uint8_t reg, uint16_t *out) {
    if (!s_i2c_dev || !out) return ESP_ERR_INVALID_ARG;
    if (!shared_i2c_bus_lock(pdMS_TO_TICKS(INA226_I2C_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(
        s_i2c_dev, &reg, 1, data, sizeof(data), INA226_I2C_TIMEOUT_MS);
    shared_i2c_bus_unlock();
    if (err != ESP_OK) return err;
    *out = (uint16_t)((data[0] << 8) | data[1]);
    return ESP_OK;
}

static void ina226_recover(const char *stage, esp_err_t err) {
    ESP_LOGW(INA_TAG, "%s err=%s", stage, esp_err_to_name(err));
    s_ina_error = true;
    if (s_i2c_bus) {
        if (!shared_i2c_bus_lock(pdMS_TO_TICKS(INA226_I2C_TIMEOUT_MS))) {
            ESP_LOGW(INA_TAG, "i2c reset lock timeout");
            return;
        }
        esp_err_t reset_err = i2c_master_bus_reset(s_i2c_bus);
        shared_i2c_bus_unlock();
        if (reset_err != ESP_OK) {
            ESP_LOGW(INA_TAG, "i2c bus reset err=%s", esp_err_to_name(reset_err));
        }
    }
}

static bool ina226_verify_reg(uint8_t reg, uint16_t expected, uint16_t mask, const char *name) {
    uint16_t actual = 0;
    esp_err_t err = ina226_read_reg(reg, &actual);
    if (err != ESP_OK) {
        ina226_recover(name, err);
        return false;
    }
    if ((actual & mask) != (expected & mask)) {
        ESP_LOGW(INA_TAG, "%s mismatch wrote=0x%04X read=0x%04X", name, expected, actual);
        s_ina_error = true;
        return false;
    }
    ESP_LOGI(INA_TAG, "%s=0x%04X", name, actual);
    return true;
}

static bool ina226_configure_device(void) {
    esp_err_t err = ina226_write_reg(REG_CONFIG, INA226_CONFIG_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write config", err);
        return false;
    }
    if (!ina226_verify_reg(REG_CONFIG, INA226_CONFIG_VALUE, INA226_CONFIG_FIELDS, "config")) return false;

    err = ina226_write_reg(REG_CALIBRATION, CAL_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write calibration", err);
        return false;
    }
    if (!ina226_verify_reg(REG_CALIBRATION, CAL_VALUE, INA226_CAL_FIELDS, "calibration")) return false;

    // Current protection consumes conversions from the cache. Disable all pin alerts.
    err = ina226_write_reg(REG_MASK_ENABLE, INA226_MASK_ENABLE_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write mask/enable", err);
        return false;
    }
    if (!ina226_verify_reg(REG_MASK_ENABLE, INA226_MASK_ENABLE_VALUE, INA226_MASK_ENABLE_FIELDS, "mask_enable")) {
        return false;
    }

    return true;
}

static bool ina226_init(void) {
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
        if (!shared_i2c_bus_lock(pdMS_TO_TICKS(INA226_I2C_TIMEOUT_MS))) {
            ESP_LOGW(INA_TAG, "i2c add device lock timeout");
            s_ina_error = true;
            return false;
        }
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
        shared_i2c_bus_unlock();
        if (err != ESP_OK) {
            ESP_LOGW(INA_TAG, "i2c add device err=%s", esp_err_to_name(err));
            s_ina_error = true;
            return false;
        }
    }

    if (!ina226_configure_device()) return false;

    s_started = true;
    s_ina_error = false;
    ESP_LOGI(INA_TAG, "initialized addr=0x%02X", INA226_ADDR);
    return true;
}

static float ina226_bus_raw_to_v(uint16_t raw) {
    return (float)raw * 1.25e-3f;
}

static float ina226_current_raw_to_ma(int16_t raw) {
    return (float)raw * 0.05f;
}

static ina226_state_t ina226_poll(ina226_snapshot_t *sample, ina226_timing_t *timing) {
    if (!s_started || s_ina_error) *timing = (ina226_timing_t){0};
    if (!ina226_init()) return INA226_OFFLINE;

    uint16_t bus_raw = 0;
    uint16_t current_raw_u = 0;

    uint16_t mask;
    int64_t previous_mask_read_us = timing->mask_read_started_us;
    timing->mask_read_started_us = esp_timer_get_time();
    esp_err_t err = ina226_read_reg(REG_MASK_ENABLE, &mask);
    if (err != ESP_OK) {
        ina226_recover("read conversion ready", err);
        return INA226_OFFLINE;
    }
    if ((mask & INA226_MASK_CVRF_BIT) == 0) return INA226_WAITING;

    // Reading CVRF clears it. This conversion ended after the previous read began,
    // and its averaging window began when the preceding conversion completed.
    sample->conversion_started_after_us = timing->conversion_completed_after_us;
    timing->conversion_completed_after_us = previous_mask_read_us;

    err = ina226_read_reg(REG_BUS_VOLTAGE, &bus_raw);
    if (err != ESP_OK) {
        ina226_recover("read bus voltage", err);
        return INA226_OFFLINE;
    }

    err = ina226_read_reg(REG_CURRENT, &current_raw_u);
    if (err != ESP_OK) {
        ina226_recover("read current", err);
        return INA226_OFFLINE;
    }

    previous_mask_read_us = timing->mask_read_started_us;
    timing->mask_read_started_us = esp_timer_get_time();
    err = ina226_read_reg(REG_MASK_ENABLE, &mask);
    if (err != ESP_OK) {
        ina226_recover("check conversion did not change during read", err);
        return INA226_OFFLINE;
    }
    // If another conversion completed during the read, the two registers may represent different batches.
    if (mask & INA226_MASK_CVRF_BIT) {
        timing->conversion_completed_after_us = previous_mask_read_us;
        sample->sequence++;
        return INA226_WAITING;
    }

    sample->voltage_v = ina226_bus_raw_to_v(bus_raw);
    sample->current_ma = ina226_current_raw_to_ma((int16_t)current_raw_u);
    sample->sampled_us = esp_timer_get_time();
    sample->sequence++;
    return INA226_READY;
}

static void ina226_monitor_task(void *arg) {
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(INA226_POLL_PERIOD_MS);
    TickType_t wake = xTaskGetTickCount();
    ina226_timing_t timing = {0};
    ina226_snapshot_t sample = {0};
    for (;;) {
        ina226_state_t state = ina226_poll(&sample, &timing);
        portENTER_CRITICAL(&s_snapshot_mux);
        s_snapshot.state = state;
        if (state == INA226_READY) {
            sample.state = state;
            sample.sample_valid = true;
            s_snapshot = sample;
        } else if (state == INA226_OFFLINE) {
            s_snapshot.sample_valid = false;
        }
        portEXIT_CRITICAL(&s_snapshot_mux);
        if (xTaskDelayUntil(&wake, period) == pdFALSE) wake = xTaskGetTickCount();
    }
}

void ina226_monitor_start(void) {
    portENTER_CRITICAL(&s_snapshot_mux);
    bool already_started = s_monitor_started;
    s_monitor_started = true;
    portEXIT_CRITICAL(&s_snapshot_mux);
    if (already_started) return;
    ESP_ERROR_CHECK(xTaskCreate(ina226_monitor_task, "ina226", 4096, NULL, 5, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}

ina226_snapshot_t ina226_get_snapshot(void) {
    portENTER_CRITICAL(&s_snapshot_mux);
    ina226_snapshot_t snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_mux);
    return snapshot;
}
