#include "ina226.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "shared_i2c_bus.h"

static const char *INA_TAG = "ina226";

#define INA226_CONFIG_VALUE      0x4D37
#define INA226_ALERT_LIMIT_VALUE 0x24B8
#define INA226_MASK_SOL_BIT      (1U << 15)
#define INA226_MASK_AFF_BIT      (1U << 4)
#define INA226_I2C_TIMEOUT_MS    100
#define INA226_POLL_PERIOD_US    (1000 * 1000LL)
#define INA226_QUEUE_DEPTH       8
#define INA226_TASK_STACK        3072
#define INA226_TASK_PRIORITY     5

typedef enum {
    INA226_EVT_POLL = 0,
    INA226_EVT_ALERT = 1,
} ina226_event_t;

static bool s_i2c_inited = false;
static bool s_ina_error = false;
static bool s_started = false;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static esp_timer_handle_t s_poll_timer = NULL;
static QueueHandle_t s_evt_queue = NULL;
static portMUX_TYPE s_sample_mux = portMUX_INITIALIZER_UNLOCKED;
static ina226_sample_t s_sample = {0};

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

static void ina226_sample_write(uint16_t bus_raw, int16_t current_raw, bool overcurrent, bool valid) {
    ina226_sample_t sample = {
        .bus_raw = bus_raw,
        .current_raw = current_raw,
        .voltage_v = ina226_bus_raw_to_v(bus_raw),
        .current_ma = ina226_current_raw_to_ma(current_raw),
        .overcurrent = overcurrent,
        .valid = valid,
    };

    portENTER_CRITICAL(&s_sample_mux);
    s_sample = sample;
    portEXIT_CRITICAL(&s_sample_mux);
}

static void ina226_mark_invalid(void) {
    portENTER_CRITICAL(&s_sample_mux);
    s_sample.valid = false;
    portEXIT_CRITICAL(&s_sample_mux);
}

static void ina226_recover(const char *stage, esp_err_t err) {
    ESP_LOGW(INA_TAG, "%s err=%s", stage, esp_err_to_name(err));
    s_ina_error = true;
    ina226_mark_invalid();
    if (s_i2c_bus) {
        esp_err_t reset_err = i2c_master_bus_reset(s_i2c_bus);
        if (reset_err != ESP_OK) {
            ESP_LOGW(INA_TAG, "i2c bus reset err=%s", esp_err_to_name(reset_err));
        }
    }
}

static bool ina226_read_sample(bool alert_event) {
    uint16_t bus_raw = 0;
    uint16_t current_raw_u = 0;
    uint16_t mask_raw = 0;

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

    err = ina226_read_reg(REG_MASK_ENABLE, &mask_raw);
    if (err != ESP_OK) {
        ina226_recover("read mask/enable", err);
        return false;
    }

    bool overcurrent = alert_event || ((mask_raw & INA226_MASK_AFF_BIT) != 0) || gpio_get_level(INA226_ALERT_GPIO) == 0;
    ina226_sample_write(bus_raw, (int16_t)current_raw_u, overcurrent, true);
    s_ina_error = false;
    return true;
}

static void ina226_poll_timer_cb(void *arg) {
    (void)arg;
    if (!s_evt_queue) return;
    ina226_event_t evt = INA226_EVT_POLL;
    (void)xQueueSend(s_evt_queue, &evt, 0);
}

static void IRAM_ATTR ina226_alert_isr(void *arg) {
    (void)arg;
    if (!s_evt_queue) return;
    BaseType_t hp_task_woken = pdFALSE;
    ina226_event_t evt = INA226_EVT_ALERT;
    xQueueSendFromISR(s_evt_queue, &evt, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void ina226_task(void *arg) {
    (void)arg;

    while (1) {
        ina226_event_t evt = INA226_EVT_POLL;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool alert_event = evt == INA226_EVT_ALERT;
        while (xQueueReceive(s_evt_queue, &evt, 0) == pdTRUE) {
            alert_event = alert_event || evt == INA226_EVT_ALERT;
        }
        (void)ina226_read_sample(alert_event);
    }
}

static bool ina226_configure_alert_gpio(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << INA226_ALERT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(INA_TAG, "alert gpio config err=%s", esp_err_to_name(err));
        return false;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(INA_TAG, "gpio_install_isr_service err=%s", esp_err_to_name(err));
        return false;
    }

    err = gpio_isr_handler_add(INA226_ALERT_GPIO, ina226_alert_isr, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(INA_TAG, "gpio_isr_handler_add err=%s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool ina226_configure_device(void) {
    esp_err_t err = ina226_write_reg(REG_CONFIG, INA226_CONFIG_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write config", err);
        return false;
    }

    uint16_t config_readback = 0;
    err = ina226_read_reg(REG_CONFIG, &config_readback);
    if (err != ESP_OK) {
        ina226_recover("read config", err);
        return false;
    }
    if (config_readback != INA226_CONFIG_VALUE) {
        ESP_LOGW(INA_TAG, "config mismatch wrote=0x%04X read=0x%04X",
                 INA226_CONFIG_VALUE, config_readback);
    } else {
        ESP_LOGI(INA_TAG, "config=0x%04X", config_readback);
    }

    err = ina226_write_reg(REG_CALIBRATION, CAL_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write calibration", err);
        return false;
    }

    err = ina226_write_reg(REG_ALERT_LIMIT, INA226_ALERT_LIMIT_VALUE);
    if (err != ESP_OK) {
        ina226_recover("write alert limit", err);
        return false;
    }

    err = ina226_write_reg(REG_MASK_ENABLE, INA226_MASK_SOL_BIT);
    if (err != ESP_OK) {
        ina226_recover("write mask/enable", err);
        return false;
    }

    return true;
}

bool ina226_init(void) {
    if (s_started) return !s_ina_error && s_i2c_dev != NULL;

    s_i2c_bus = shared_i2c_bus_get_handle();
    if (s_i2c_bus == NULL) {
        ESP_LOGW(INA_TAG, "shared i2c bus is not initialized");
        s_ina_error = true;
        return false;
    }

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

    if (!ina226_configure_device()) return false;
    if (!ina226_configure_alert_gpio()) return false;

    s_evt_queue = xQueueCreate(INA226_QUEUE_DEPTH, sizeof(ina226_event_t));
    if (!s_evt_queue) {
        ESP_LOGW(INA_TAG, "event queue allocation failed");
        s_ina_error = true;
        return false;
    }

    BaseType_t task_ok = xTaskCreate(
        ina226_task, "ina226", INA226_TASK_STACK, NULL, INA226_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGW(INA_TAG, "task create failed");
        s_ina_error = true;
        return false;
    }

    esp_timer_create_args_t timer_args = {
        .callback = ina226_poll_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ina226_poll",
    };
    err = esp_timer_create(&timer_args, &s_poll_timer);
    if (err != ESP_OK) {
        ESP_LOGW(INA_TAG, "timer create err=%s", esp_err_to_name(err));
        s_ina_error = true;
        return false;
    }

    err = esp_timer_start_periodic(s_poll_timer, INA226_POLL_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGW(INA_TAG, "timer start err=%s", esp_err_to_name(err));
        s_ina_error = true;
        return false;
    }

    s_started = true;
    s_ina_error = false;
    (void)ina226_read_sample(false);
    ESP_LOGI(INA_TAG, "initialized addr=0x%02X alert_gpio=%d", INA226_ADDR, INA226_ALERT_GPIO);
    return true;
}

bool ina226_has_error(void) {
    return s_ina_error || (s_i2c_inited && s_i2c_dev == NULL);
}

bool ina226_get_sample(ina226_sample_t *out) {
    if (!out) return false;
    portENTER_CRITICAL(&s_sample_mux);
    *out = s_sample;
    portEXIT_CRITICAL(&s_sample_mux);
    return out->valid;
}

bool ina226_overcurrent_active(void) {
    bool overcurrent = false;
    portENTER_CRITICAL(&s_sample_mux);
    overcurrent = s_sample.valid && s_sample.overcurrent;
    portEXIT_CRITICAL(&s_sample_mux);
    return overcurrent;
}

float ina226_bus_raw_to_v(uint16_t raw) {
    return (float)raw * 1.25e-3f;
}

float ina226_current_raw_to_ma(int16_t raw) {
    return (float)raw * 0.05f;
}
