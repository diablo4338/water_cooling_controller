#include "runtime_config.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"

#define CONFIG_NVS_NAMESPACE "runtime_cfg"
#define CONFIG_NVS_KEY "values"

static const char *CONFIG_TAG = "runtime-config";
static portMUX_TYPE config_mux = portMUX_INITIALIZER_UNLOCKED;
static runtime_config_t current_config;

static const runtime_config_t default_config = {
    .idle_current_ma = 100,
    .voltage_min_mv = 0,
    .voltage_max_mv = 36000,
    .idle_timeout_s = 300,
    .button_press_deci_c = 300,
    .button_release_deci_c = 290,
    .deep_sleep_ms = 5000,
};

static uint16_t read_u16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t read_i16(const uint8_t *data) {
    return (int16_t)read_u16(data);
}

static uint32_t read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void encode_config(const runtime_config_t *config, uint16_t request_id,
                          uint8_t out[RUNTIME_CONFIG_WIRE_SIZE]) {
    out[0] = RUNTIME_CONFIG_VERSION;
    write_u16(out + 1, request_id);
    write_u16(out + 3, config->idle_current_ma);
    write_u16(out + 5, config->voltage_min_mv);
    write_u16(out + 7, config->voltage_max_mv);
    write_u32(out + 9, config->idle_timeout_s);
    write_u16(out + 13, (uint16_t)config->button_press_deci_c);
    write_u16(out + 15, (uint16_t)config->button_release_deci_c);
    write_u16(out + 17, config->deep_sleep_ms);
}

static bool decode_config(const uint8_t *data, size_t length,
                          runtime_config_t *config, uint16_t *request_id) {
    if (!data || !config || length != RUNTIME_CONFIG_WIRE_SIZE ||
        data[0] != RUNTIME_CONFIG_VERSION) {
        return false;
    }
    if (request_id) *request_id = read_u16(data + 1);
    *config = (runtime_config_t) {
        .idle_current_ma = read_u16(data + 3),
        .voltage_min_mv = read_u16(data + 5),
        .voltage_max_mv = read_u16(data + 7),
        .idle_timeout_s = read_u32(data + 9),
        .button_press_deci_c = read_i16(data + 13),
        .button_release_deci_c = read_i16(data + 15),
        .deep_sleep_ms = read_u16(data + 17),
    };
    return true;
}

static runtime_config_status_t validate_config(const runtime_config_t *config,
                                               runtime_config_field_t *field) {
    *field = CONFIG_FIELD_NONE;
    if (config->idle_current_ma > 1600) {
        *field = CONFIG_FIELD_IDLE_CURRENT;
        return CONFIG_STATUS_RANGE;
    }
    if (config->voltage_min_mv > 36000) {
        *field = CONFIG_FIELD_VOLTAGE_MIN;
        return CONFIG_STATUS_RANGE;
    }
    if (config->voltage_max_mv > 36000) {
        *field = CONFIG_FIELD_VOLTAGE_MAX;
        return CONFIG_STATUS_RANGE;
    }
    if (config->idle_timeout_s == 0 || config->idle_timeout_s > 86400) {
        *field = CONFIG_FIELD_IDLE_TIMEOUT;
        return CONFIG_STATUS_RANGE;
    }
    if (config->button_press_deci_c < -400 || config->button_press_deci_c > 1250) {
        *field = CONFIG_FIELD_BUTTON_PRESS;
        return CONFIG_STATUS_RANGE;
    }
    if (config->button_release_deci_c < -400 || config->button_release_deci_c > 1250) {
        *field = CONFIG_FIELD_BUTTON_RELEASE;
        return CONFIG_STATUS_RANGE;
    }
    if (config->deep_sleep_ms < 100 || config->deep_sleep_ms > 60000) {
        *field = CONFIG_FIELD_DEEP_SLEEP;
        return CONFIG_STATUS_RANGE;
    }
    if (config->button_press_deci_c <= config->button_release_deci_c) {
        *field = CONFIG_FIELD_BUTTON_PRESS;
        return CONFIG_STATUS_BUTTON_ORDER;
    }
    if (config->voltage_max_mv <= config->voltage_min_mv) {
        *field = CONFIG_FIELD_VOLTAGE_MAX;
        return CONFIG_STATUS_VOLTAGE_ORDER;
    }
    return CONFIG_STATUS_OK;
}

void runtime_config_init(void) {
    current_config = default_config;
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(CONFIG_TAG, "No stored config; using defaults");
        return;
    }
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    size_t length = sizeof(wire);
    esp_err_t err = nvs_get_blob(handle, CONFIG_NVS_KEY, wire, &length);
    nvs_close(handle);
    runtime_config_t loaded;
    runtime_config_field_t field;
    if (err != ESP_OK || !decode_config(wire, length, &loaded, NULL) ||
        validate_config(&loaded, &field) != CONFIG_STATUS_OK) {
        ESP_LOGW(CONFIG_TAG, "Stored config missing or invalid; using defaults");
        return;
    }
    current_config = loaded;
    ESP_LOGI(CONFIG_TAG, "Runtime config loaded from NVS");
}

void runtime_config_get(runtime_config_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&config_mux);
    *out = current_config;
    portEXIT_CRITICAL(&config_mux);
}

void runtime_config_encode(uint8_t out[RUNTIME_CONFIG_WIRE_SIZE], uint16_t request_id) {
    runtime_config_t config;
    runtime_config_get(&config);
    encode_config(&config, request_id, out);
}

void runtime_config_result_encode(const runtime_config_result_t *result,
                                  uint8_t out[RUNTIME_CONFIG_RESULT_SIZE]) {
    out[0] = RUNTIME_CONFIG_VERSION;
    write_u16(out + 1, result->request_id);
    out[3] = (uint8_t)result->status;
    out[4] = (uint8_t)result->field;
}

void runtime_config_apply_wire(const uint8_t *data, size_t length,
                               runtime_config_result_t *result) {
    if (!result) return;
    *result = (runtime_config_result_t) {
        .request_id = data && length >= 3 ? read_u16(data + 1) : 0,
        .status = CONFIG_STATUS_FORMAT,
        .field = CONFIG_FIELD_NONE,
    };
    runtime_config_t candidate;
    if (!decode_config(data, length, &candidate, &result->request_id)) return;
    result->status = validate_config(&candidate, &result->field);
    if (result->status != CONFIG_STATUS_OK) return;

    uint8_t stored[RUNTIME_CONFIG_WIRE_SIZE];
    encode_config(&candidate, 0, stored);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_blob(handle, CONFIG_NVS_KEY, stored, sizeof(stored));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (err != ESP_OK) {
        result->status = CONFIG_STATUS_FLASH;
        ESP_LOGE(CONFIG_TAG, "Config NVS commit failed: %s", esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&config_mux);
    current_config = candidate;
    portEXIT_CRITICAL(&config_mux);
    ESP_LOGI(CONFIG_TAG, "Runtime config committed request=%u", (unsigned)result->request_id);
}
