#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "device_status.h"
#include "ina226.h"
#include "nvs.h"
#include "output_limit.h"
#include "params.h"
#include "thermal_protection.h"

const char *TAG = "test-profile";

static uint8_t config_record[64];
static size_t record_size;
static uint8_t storage_started;
static bool fail_write;

void state_lock(void) {}
void state_unlock(void) {}
void device_status_set_error_flag(device_error_mask_t flag, bool active) {}
void device_status_update_error_flags(device_error_mask_t owned, device_error_mask_t active) {}
thermal_input_state_t thermal_protection_get_input(void) { return THERMAL_INPUT_READY; }
ina226_snapshot_t ina226_get_snapshot(void) {
    return (ina226_snapshot_t){0};
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(strcmp(name, "params") == 0);
    *handle = 1;
    return mode == NVS_READONLY && !storage_started ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len) {
    if (strcmp(key, "config") != 0 || record_size == 0) return ESP_ERR_NVS_NOT_FOUND;
    assert(record_size <= *len);
    memcpy(out, config_record, record_size);
    *len = record_size;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
    if (strcmp(key, "config_started") != 0 || !storage_started) return ESP_ERR_NVS_NOT_FOUND;
    *out = storage_started;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    assert(strcmp(key, "config_started") == 0);
    storage_started = value;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len) {
    assert(strcmp(key, "config") == 0 && len <= sizeof(config_record));
    if (fail_write) return -1;
    memcpy(config_record, data, len);
    record_size = len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { return ESP_OK; }
void nvs_close(nvs_handle_t handle) {}

static uint8_t apply_minimum(int32_t speed) {
    uint8_t payload[PARAMS_PAYLOAD_LEN];
    assert(params_get_current_payload(payload, sizeof(payload)));
    memcpy(payload + 3, &speed, sizeof(speed));
    assert(params_set_pending_payload(1, payload, sizeof(payload)));
    return params_apply(1);
}

int main(void) {
    params_init();
    assert(apply_minimum(100) == PARAM_STATUS_OK);
    uint8_t profile[PARAMS_PAYLOAD_LEN];
    assert(params_get_current_payload(profile, sizeof(profile)));
    uint8_t original[PARAMS_PAYLOAD_LEN];
    memcpy(original, profile, sizeof(original));

    assert(apply_minimum(80) == PARAM_STATUS_OK);
    fail_write = true;
    assert(params_set_output_max(70) == PARAM_STATUS_STORAGE_ERROR);
    params_config_t config = params_read_config();
    assert(config.params.fan_min_speed == 80 && config.maximum == 100);
    fail_write = false;
    assert(params_set_output_max(70) == PARAM_STATUS_OK);
    config = params_read_config();
    assert(config.params.fan_min_speed == 70 && config.maximum == 70);
    assert(params_set_output_max(9) == PARAM_STATUS_INVALID);
    assert(params_get_output_max() == 70);
    output_limit_command_t command = output_limit_step(80.0f, 80.0f, config.params.fan_control_type,
                                                       config.maximum, config.params.fan_min_speed, OUTPUT_LIMIT_MODE_CONTINUOUS, 4000000);
    assert(command.percent == 70.0f);

    // The maximum is already saved before the regular Apply changes the minimum.
    assert(apply_minimum(60) == PARAM_STATUS_OK);
    config = params_read_config();
    assert(config.maximum == 70 && config.params.fan_min_speed == 60);
    assert(output_limit_step(config.params.fan_min_speed, 60.0f, config.params.fan_control_type,
                            config.maximum, config.params.fan_min_speed, OUTPUT_LIMIT_MODE_CONTINUOUS, 4050000).percent == 60.0f);
    assert(apply_minimum(80) == PARAM_STATUS_OK);
    config = params_read_config();
    assert(config.maximum == 70 && config.params.fan_min_speed == 70);
    assert(output_limit_step(config.params.fan_min_speed, 60.0f, config.params.fan_control_type,
                            config.maximum, config.params.fan_min_speed, OUTPUT_LIMIT_MODE_CONTINUOUS, 4100000).percent == 70.0f);
    params_init();
    assert(params_read_config().params.fan_min_speed == 70);
    params_profile_result_t result;
    assert(params_apply_profile_payload(profile, sizeof(profile), &result));
    assert(result.status == PARAM_STATUS_OK);
    assert(result.applied_mask & PARAM_MASK_FAN_MIN_SPEED);
    assert(result.skipped[PARAM_FIELD_FAN_MIN_SPEED] == PROFILE_SKIP_OUTPUT_MAX);
    assert(memcmp(profile, original, sizeof(profile)) == 0);
    assert(apply_minimum(101) == PARAM_STATUS_INVALID);
    assert(apply_minimum(9) == PARAM_STATUS_INVALID);

    params_init();
    config = params_read_config();
    assert(config.params.fan_min_speed == 70 && config.maximum == 70);
    command = output_limit_step(100.0f, 0.0f, config.params.fan_control_type,
                                config.maximum, config.params.fan_min_speed, OUTPUT_LIMIT_MODE_CONTINUOUS, 4200000);
    assert(command.percent == 70.0f);

    assert(params_set_output_max(100) == PARAM_STATUS_OK);
    assert(params_apply_profile_payload(profile, sizeof(profile), &result));
    assert(result.status == PARAM_STATUS_OK && result.skipped[PARAM_FIELD_FAN_MIN_SPEED] == PROFILE_SKIP_NONE);
    assert(params_read_config().params.fan_min_speed == 100);
    puts("Profile application and saved output ceiling tests passed");
}
