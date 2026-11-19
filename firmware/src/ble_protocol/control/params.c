#include "params.h"

#include <string.h>

#include "device_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "output_limit.h"
#include "state.h"
#include "thermal_protection.h"

#define PARAMS_NS "params"
#define PARAMS_KEY_VER "ver"
#define PARAMS_KEY_BLOB "blob"
#define PARAMS_KEY_OUTPUT_MAX "output_max"
#define PARAMS_KEY_CONFIG "config"
#define PARAMS_KEY_CONFIG_STARTED "config_started"
#define PARAMS_OUTPUT_MAX_DEFAULT 100
#define PARAMS_VER_VALUE 6
#define PARAMS_CONFIG_VERSION 1

typedef struct {
    uint32_t version;
    params_t params;
    uint8_t maximum;
} params_record_t;

typedef struct {
    uint8_t status;
    uint8_t field;
} params_status_t;

#define PARAMS_DEFAULTS_INIT { \
    .fan_min_speed = 50, \
    .fan_control_type = PARAM_FAN_CONTROL_DC, \
    .fan_max_temp = 45, \
    .fan_off_delta = 2, \
    .fan_start_temp = 35, \
    .fan_mode = PARAM_FAN_MODE_CONTINUOUS, \
    .fan_monitoring_enabled = 1, \
    .fan2_monitoring_enabled = 1, \
    .fan3_monitoring_enabled = 1, \
    .fan4_monitoring_enabled = 1, \
    .overheat_alarm_enabled = 1, \
}

static params_config_t g_config = {
    .params = PARAMS_DEFAULTS_INIT,
    .maximum = PARAMS_OUTPUT_MAX_DEFAULT,
};
static params_t g_pending;
static uint16_t g_pending_mask = 0;
static uint16_t g_pending_owner = BLE_HS_CONN_HANDLE_NONE;
static params_status_t g_last_status = {PARAM_STATUS_OK, PARAM_FIELD_NONE};
static uint32_t g_reset_revision;
static StaticSemaphore_t g_commit_mutex_storage;
static SemaphoreHandle_t g_commit_mutex;

static params_t params_defaults(void) {
    params_t params = PARAMS_DEFAULTS_INIT;
    return params;
}

static bool params_validate(const params_t *params, uint8_t maximum, uint8_t *field);

static bool params_load_legacy(nvs_handle_t h, params_record_t *record) {
    uint32_t ver;
    size_t len = sizeof(params_t);
    params_t params;
    esp_err_t version_err = nvs_get_u32(h, PARAMS_KEY_VER, &ver);
    esp_err_t params_err = nvs_get_blob(h, PARAMS_KEY_BLOB, &params, &len);
    bool saved_params = version_err == ESP_OK && ver == PARAMS_VER_VALUE &&
                        params_err == ESP_OK && len == sizeof(params_t);
    if (saved_params && params_validate(&params, PARAMS_OUTPUT_MAX_DEFAULT, NULL)) {
        record->params = params;
    } else if (version_err != ESP_ERR_NVS_NOT_FOUND || params_err != ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }

    uint8_t maximum;
    esp_err_t err = nvs_get_u8(h, PARAMS_KEY_OUTPUT_MAX, &maximum);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) return false;
    if (!params_validate(&record->params, maximum, NULL) || maximum > PARAMS_OUTPUT_MAX_DEFAULT) {
        return false;
    }
    record->maximum = maximum;
    return true;
}

static bool params_load_from_nvs(params_record_t *record) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;

    params_record_t saved;
    size_t len = sizeof(saved);
    err = nvs_get_blob(h, PARAMS_KEY_CONFIG, &saved, &len);
    bool loaded;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t started;
        loaded = nvs_get_u8(h, PARAMS_KEY_CONFIG_STARTED, &started) == ESP_ERR_NVS_NOT_FOUND &&
                 params_load_legacy(h, record);
    } else {
        loaded = err == ESP_OK && len == sizeof(saved) && saved.version == PARAMS_CONFIG_VERSION &&
                 saved.maximum <= PARAMS_OUTPUT_MAX_DEFAULT && params_validate(&saved.params, saved.maximum, NULL);
        if (loaded) *record = saved;
    }
    nvs_close(h);
    return loaded;
}

static bool params_save_to_nvs(const params_t *params, uint8_t maximum) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Params NVS open failed: %d", err);
        return false;
    }
    const params_record_t record = {
        .version = PARAMS_CONFIG_VERSION,
        .params = *params,
        .maximum = maximum,
    };
    // NVS can report a damaged blob as missing. Mark the new format before
    // its first write so a missing record cannot reactivate legacy limits.
    uint8_t started;
    if (nvs_get_u8(h, PARAMS_KEY_CONFIG_STARTED, &started) != ESP_OK || started != 1) {
        err = nvs_set_u8(h, PARAMS_KEY_CONFIG_STARTED, 1);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    if (err == ESP_OK) err = nvs_set_blob(h, PARAMS_KEY_CONFIG, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Params NVS save failed: %d", err);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool params_decode_payload(const uint8_t *data, size_t len, params_t *out, uint16_t *mask) {
    if (len != PARAMS_PAYLOAD_LEN) return false;
    if (data[0] != PARAMS_VERSION) return false;
    if (out == NULL || mask == NULL) return false;

    uint16_t raw_mask = 0;
    memcpy(&raw_mask, data + 1, sizeof(raw_mask));
    *mask = raw_mask & PARAM_MASK_ALL;
    memcpy(&out->fan_min_speed, data + 3, 4);
    out->fan_control_type = data[7];
    memcpy(&out->fan_max_temp, data + 8, 4);
    memcpy(&out->fan_off_delta, data + 12, 4);
    memcpy(&out->fan_start_temp, data + 16, 4);
    out->fan_mode = data[20];
    out->fan_monitoring_enabled = data[21];
    out->fan2_monitoring_enabled = data[22];
    out->fan3_monitoring_enabled = data[23];
    out->fan4_monitoring_enabled = data[24];
    out->overheat_alarm_enabled = data[25];
    return true;
}

static void params_encode_payload(const params_t *params, uint16_t mask, uint8_t *out) {
    uint16_t raw_mask = mask & PARAM_MASK_ALL;
    out[0] = PARAMS_VERSION;
    memcpy(out + 1, &raw_mask, sizeof(raw_mask));
    memcpy(out + 3, &params->fan_min_speed, 4);
    out[7] = params->fan_control_type;
    memcpy(out + 8, &params->fan_max_temp, 4);
    memcpy(out + 12, &params->fan_off_delta, 4);
    memcpy(out + 16, &params->fan_start_temp, 4);
    out[20] = params->fan_mode;
    out[21] = params->fan_monitoring_enabled;
    out[22] = params->fan2_monitoring_enabled;
    out[23] = params->fan3_monitoring_enabled;
    out[24] = params->fan4_monitoring_enabled;
    out[25] = params->overheat_alarm_enabled;
}

static void set_last_status_locked(uint8_t status, uint8_t field) {
    g_last_status.status = status;
    g_last_status.field = field;
}

void params_init(void) {
    g_commit_mutex = xSemaphoreCreateMutexStatic(&g_commit_mutex_storage);
    params_record_t record = {.params = params_defaults(), .maximum = PARAMS_OUTPUT_MAX_DEFAULT};
    bool loaded = params_load_from_nvs(&record);
    state_lock();
    g_config = (params_config_t){.params = record.params, .maximum = loaded ? record.maximum : 0};
    g_pending_owner = BLE_HS_CONN_HANDLE_NONE;
    g_pending_mask = 0;
    set_last_status_locked(loaded ? PARAM_STATUS_OK : PARAM_STATUS_STORAGE_ERROR, PARAM_FIELD_NONE);
    state_unlock();
    device_status_set_error_flag(DEVICE_ERROR_CONFIG_STORAGE, !loaded);
    if (!loaded) {
        ESP_LOGE(TAG, "Failed to load configuration; output blocked until the maximum is saved");
    }
}

bool params_read(params_t *out) {
    if (out == NULL) return false;
    *out = params_read_config().params;
    return true;
}

params_config_t params_read_config(void) {
    state_lock();
    params_config_t config = g_config;
    state_unlock();
    return config;
}

bool params_set_pending_payload(uint16_t conn_handle, const uint8_t *data, size_t len) {
    params_t draft;
    uint16_t mask;
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        !params_decode_payload(data, len, &draft, &mask)) return false;
    state_lock();
    g_pending = draft;
    g_pending_mask = mask;
    g_pending_owner = conn_handle;
    state_unlock();
    return true;
}

void params_discard_pending(uint16_t conn_handle) {
    state_lock();
    if (g_pending_owner == conn_handle) {
        g_pending_owner = BLE_HS_CONN_HANDLE_NONE;
        g_pending_mask = 0;
    }
    state_unlock();
}

bool params_get_current_payload(uint8_t *out, size_t len) {
    if (out == NULL || len < PARAMS_PAYLOAD_LEN) return false;
    params_t current;
    if (!params_read(&current)) return false;
    params_encode_payload(&current, PARAM_MASK_ALL, out);
    return true;
}

void params_get_last_status_payload(uint8_t *out, size_t len) {
    if (out == NULL || len < PARAMS_STATUS_LEN) return;
    params_status_t st;
    state_lock();
    st = g_last_status;
    state_unlock();
    out[0] = PARAMS_VERSION;
    out[1] = st.status;
    out[2] = st.field;
}

void params_set_last_status(uint8_t status, uint8_t field) {
    state_lock();
    set_last_status_locked(status, field);
    state_unlock();
}

static bool params_validate(const params_t *params, uint8_t maximum, uint8_t *field) {
    if (params->fan_min_speed < 10 || params->fan_min_speed > maximum) {
        if (field) *field = PARAM_FIELD_FAN_MIN_SPEED;
        return false;
    }
    if (params->fan_control_type != PARAM_FAN_CONTROL_DC &&
        params->fan_control_type != PARAM_FAN_CONTROL_PWM) {
        if (field) *field = PARAM_FIELD_FAN_CONTROL_TYPE;
        return false;
    }
    if (params->fan_start_temp < 0 || params->fan_start_temp > 150) {
        if (field) *field = PARAM_FIELD_FAN_START_TEMP;
        return false;
    }
    if (params->fan_max_temp < 0 || params->fan_max_temp > 150) {
        if (field) *field = PARAM_FIELD_FAN_MAX_TEMP;
        return false;
    }
    if (params->fan_off_delta < 0 || params->fan_off_delta > 150) {
        if (field) *field = PARAM_FIELD_FAN_OFF_DELTA;
        return false;
    }
    if (params->fan_max_temp <= params->fan_start_temp) {
        if (field) *field = PARAM_FIELD_FAN_MAX_TEMP;
        return false;
    }
    if (params->fan_off_delta >= params->fan_start_temp) {
        if (field) *field = PARAM_FIELD_FAN_OFF_DELTA;
        return false;
    }
    if (params->fan_mode != PARAM_FAN_MODE_CONTINUOUS &&
        params->fan_mode != PARAM_FAN_MODE_TEMP_SENSOR &&
        params->fan_mode != PARAM_FAN_MODE_INACTIVE) {
        if (field) *field = PARAM_FIELD_FAN_MODE;
        return false;
    }
    if (params->fan2_monitoring_enabled > 1) {
        if (field) *field = PARAM_FIELD_FAN2_MONITORING_ENABLED;
        return false;
    }
    if (params->fan3_monitoring_enabled > 1) {
        if (field) *field = PARAM_FIELD_FAN3_MONITORING_ENABLED;
        return false;
    }
    if (params->fan4_monitoring_enabled > 1) {
        if (field) *field = PARAM_FIELD_FAN4_MONITORING_ENABLED;
        return false;
    }
    if (params->overheat_alarm_enabled > 1) {
        if (field) *field = PARAM_FIELD_OVERHEAT_ALARM_ENABLED;
        return false;
    }
    return true;
}

static params_t params_merge(params_t current, const params_t *pending, uint16_t mask) {
    params_t candidate = current;
    if (mask & PARAM_MASK_FAN_MIN_SPEED) candidate.fan_min_speed = pending->fan_min_speed;
    if (mask & PARAM_MASK_FAN_CONTROL_TYPE) candidate.fan_control_type = pending->fan_control_type;
    if (mask & PARAM_MASK_FAN_MAX_TEMP) candidate.fan_max_temp = pending->fan_max_temp;
    if (mask & PARAM_MASK_FAN_OFF_DELTA) candidate.fan_off_delta = pending->fan_off_delta;
    if (mask & PARAM_MASK_FAN_START_TEMP) candidate.fan_start_temp = pending->fan_start_temp;
    if (mask & PARAM_MASK_FAN_MODE) candidate.fan_mode = pending->fan_mode;
    if (mask & PARAM_MASK_FAN_MONITORING_ENABLED) candidate.fan_monitoring_enabled = pending->fan_monitoring_enabled;
    if (mask & PARAM_MASK_FAN2_MONITORING_ENABLED) candidate.fan2_monitoring_enabled = pending->fan2_monitoring_enabled;
    if (mask & PARAM_MASK_FAN3_MONITORING_ENABLED) candidate.fan3_monitoring_enabled = pending->fan3_monitoring_enabled;
    if (mask & PARAM_MASK_FAN4_MONITORING_ENABLED) candidate.fan4_monitoring_enabled = pending->fan4_monitoring_enabled;
    if (mask & PARAM_MASK_OVERHEAT_ALARM_ENABLED) candidate.overheat_alarm_enabled = pending->overheat_alarm_enabled;
    return candidate;
}

static uint8_t params_commit(params_t candidate, uint8_t maximum, bool consume_pending, uint8_t *field_id) {
    candidate.fan_monitoring_enabled = 1;
    if (maximum == 0) {
        params_set_last_status(PARAM_STATUS_STORAGE_ERROR, PARAM_FIELD_NONE);
        if (field_id) *field_id = PARAM_FIELD_NONE;
        return PARAM_STATUS_STORAGE_ERROR;
    }

    uint8_t field = PARAM_FIELD_NONE;
    if (!params_validate(&candidate, maximum, &field)) {
        state_lock();
        set_last_status_locked(PARAM_STATUS_INVALID, field);
        state_unlock();
        if (field_id) *field_id = field;
        return PARAM_STATUS_INVALID;
    }

    if (!output_limit_begin_config_write()) {
        params_set_last_status(PARAM_STATUS_OUTPUT_ADJUSTING, PARAM_FIELD_NONE);
        if (field_id) *field_id = PARAM_FIELD_NONE;
        return PARAM_STATUS_OUTPUT_ADJUSTING;
    }

    if (!params_save_to_nvs(&candidate, maximum)) {
        output_limit_end_config_write();
        state_lock();
        set_last_status_locked(PARAM_STATUS_STORAGE_ERROR, PARAM_FIELD_NONE);
        state_unlock();
        if (field_id) *field_id = PARAM_FIELD_NONE;
        return PARAM_STATUS_STORAGE_ERROR;
    }

    state_lock();
    bool recovered = g_config.maximum == 0;
    g_config = (params_config_t){.params = candidate, .maximum = maximum};
    if (consume_pending) {
        g_pending_owner = BLE_HS_CONN_HANDLE_NONE;
        g_pending_mask = 0;
    }
    set_last_status_locked(PARAM_STATUS_OK, PARAM_FIELD_NONE);
    state_unlock();
    if (recovered) device_status_set_error_flag(DEVICE_ERROR_CONFIG_STORAGE, false);
    output_limit_end_config_write();

    if (field_id) *field_id = PARAM_FIELD_NONE;
    return PARAM_STATUS_OK;
}

static uint8_t params_check_temperature_mode(void) {
    thermal_input_state_t input = thermal_protection_get_input();
    if (input == THERMAL_INPUT_READY) return PARAM_STATUS_OK;
    return input == THERMAL_INPUT_NTC_UNAVAILABLE ? PARAM_STATUS_NTC_UNAVAILABLE : PARAM_STATUS_ADC_UNAVAILABLE;
}

static bool params_cap_requested_minimum(params_t *params, uint8_t maximum) {
    if (maximum == 0 || params->fan_min_speed <= maximum || params->fan_min_speed > PARAMS_OUTPUT_MAX_DEFAULT) {
        return false;
    }
    params->fan_min_speed = maximum;
    return true;
}

uint8_t params_apply(uint16_t conn_handle) {
    xSemaphoreTake(g_commit_mutex, portMAX_DELAY);
    state_lock();
    params_t current = g_config.params;
    params_t pending = g_pending;
    uint16_t mask = g_pending_mask;
    bool owns_draft = conn_handle != BLE_HS_CONN_HANDLE_NONE && g_pending_owner == conn_handle;
    state_unlock();

    uint8_t status;
    if (owns_draft && output_limit_settings_locked()) {
        status = PARAM_STATUS_OUTPUT_ADJUSTING;
        params_set_last_status(status, PARAM_FIELD_NONE);
    } else if (owns_draft) {
        params_t candidate = params_merge(current, &pending, mask);
        int32_t requested_speed = candidate.fan_min_speed;
        bool speed_limited = (mask & PARAM_MASK_FAN_MIN_SPEED) &&
                             params_cap_requested_minimum(&candidate, g_config.maximum);
        status = candidate.fan_mode == PARAM_FAN_MODE_TEMP_SENSOR &&
                 current.fan_mode != PARAM_FAN_MODE_TEMP_SENSOR ? params_check_temperature_mode() : PARAM_STATUS_OK;
        if (status == PARAM_STATUS_OK) {
            status = params_commit(candidate, g_config.maximum, true, NULL);
            if (status == PARAM_STATUS_OK && speed_limited) {
                ESP_LOGW(TAG, "Applied minimum limited from %ld%% to %ld%% by saved maximum",
                         (long)requested_speed, (long)candidate.fan_min_speed);
            }
        } else {
            params_set_last_status(status, PARAM_FIELD_FAN_MODE);
        }
    } else {
        status = PARAM_STATUS_INVALID;
        params_set_last_status(status, PARAM_FIELD_NONE);
    }
    xSemaphoreGive(g_commit_mutex);
    return status;
}

static uint8_t params_commit_changes(const params_t *changes, uint16_t mask, uint8_t *field_id) {
    params_t current;
    params_read(&current);
    return params_commit(params_merge(current, changes, mask), g_config.maximum, false, field_id);
}

uint32_t params_reset_revision(void) {
    return __atomic_load_n(&g_reset_revision, __ATOMIC_ACQUIRE);
}

uint8_t params_apply_operation_result(const params_t *changes, uint16_t mask, uint32_t revision) {
    xSemaphoreTake(g_commit_mutex, portMAX_DELAY);
    uint8_t status = params_reset_revision() == revision
                         ? params_commit_changes(changes, mask, NULL)
                         : PARAM_STATUS_BUSY;
    xSemaphoreGive(g_commit_mutex);
    return status;
}

bool params_apply_profile_payload(const uint8_t *data, size_t len, params_profile_result_t *result) {
    params_t changes;
    uint16_t allowed;
    if (!result || !params_decode_payload(data, len, &changes, &allowed)) return false;

    *result = (params_profile_result_t){.status = PARAM_STATUS_OK};
    xSemaphoreTake(g_commit_mutex, portMAX_DELAY);
    if (g_config.maximum == 0) {
        result->status = PARAM_STATUS_STORAGE_ERROR;
        params_set_last_status(result->status, PARAM_FIELD_NONE);
        xSemaphoreGive(g_commit_mutex);
        return true;
    }
    params_t current;
    params_read(&current);
    if (output_limit_settings_locked()) {
        result->status = PARAM_STATUS_OUTPUT_ADJUSTING;
        params_set_last_status(result->status, PARAM_FIELD_NONE);
        xSemaphoreGive(g_commit_mutex);
        return true;
    }
    if ((allowed & PARAM_MASK_FAN_MODE) && changes.fan_mode == PARAM_FAN_MODE_TEMP_SENSOR) {
        uint8_t status = params_check_temperature_mode();
        if (status != PARAM_STATUS_OK) {
            allowed &= ~PARAM_MASK_FAN_MODE;
            result->skipped[PARAM_FIELD_FAN_MODE] = status == PARAM_STATUS_NTC_UNAVAILABLE
                                                       ? PROFILE_SKIP_NTC : PROFILE_SKIP_ADC;
        }
    }

    if ((allowed & PARAM_MASK_FAN_MONITORING_ENABLED) && changes.fan_monitoring_enabled != 1) {
        allowed &= ~PARAM_MASK_FAN_MONITORING_ENABLED;
        result->skipped[PARAM_FIELD_FAN_MONITORING_ENABLED] = PROFILE_SKIP_INVALID;
    }

    int32_t requested_speed = changes.fan_min_speed;
    bool speed_limited = (allowed & PARAM_MASK_FAN_MIN_SPEED) &&
                         params_cap_requested_minimum(&changes, g_config.maximum);

    const uint16_t temp_fields = PARAM_MASK_FAN_START_TEMP | PARAM_MASK_FAN_MAX_TEMP | PARAM_MASK_FAN_OFF_DELTA;
    while (allowed) {
        params_t candidate = params_merge(current, &changes, allowed);
        uint8_t field = PARAM_FIELD_NONE;
        if (params_validate(&candidate, g_config.maximum, &field)) {
            result->status = params_commit(candidate, g_config.maximum, false, NULL);
            if (result->status == PARAM_STATUS_OK) {
                result->applied_mask = allowed;
                if (speed_limited && (allowed & PARAM_MASK_FAN_MIN_SPEED)) {
                    result->skipped[PARAM_FIELD_FAN_MIN_SPEED] = PROFILE_SKIP_OUTPUT_MAX;
                    ESP_LOGW(TAG, "Profile minimum limited from %ld%% to %ld%% by saved maximum",
                             (long)requested_speed, (long)changes.fan_min_speed);
                }
            }
            break;
        }
        if (field >= PARAM_FIELD_COUNT) {
            result->status = PARAM_STATUS_INVALID;
            break;
        }
        uint16_t rejected = (uint16_t)(1U << field) & allowed;
        uint8_t reason = PROFILE_SKIP_INVALID;
        if ((1U << field) & temp_fields) {
            int32_t value = field == PARAM_FIELD_FAN_START_TEMP ? candidate.fan_start_temp :
                            field == PARAM_FIELD_FAN_MAX_TEMP ? candidate.fan_max_temp : candidate.fan_off_delta;
            bool out_of_range = value < 0 || value > 150;
            if (!out_of_range || rejected == 0) {
                rejected = temp_fields & allowed;
                reason = PROFILE_SKIP_TEMP_CONFLICT;
            }
        }
        if (!rejected) {
            result->status = PARAM_STATUS_INVALID;
            break;
        }
        for (uint8_t i = 0; i < PARAM_FIELD_COUNT; i++) {
            if (rejected & (1U << i)) result->skipped[i] = reason;
        }
        allowed &= ~rejected;
    }
    params_set_last_status(result->status, PARAM_FIELD_NONE);
    xSemaphoreGive(g_commit_mutex);
    return true;
}

uint8_t params_get_output_max(void) {
    return params_read_config().maximum;
}

uint8_t params_set_output_max(uint8_t maximum) {
    xSemaphoreTake(g_commit_mutex, portMAX_DELAY);
    if (maximum == 0 || maximum > PARAMS_OUTPUT_MAX_DEFAULT) {
        params_set_last_status(PARAM_STATUS_INVALID, PARAM_FIELD_NONE);
        xSemaphoreGive(g_commit_mutex);
        return PARAM_STATUS_INVALID;
    }

    params_t candidate;
    params_read(&candidate);
    int32_t previous_minimum = candidate.fan_min_speed;
    if (candidate.fan_min_speed > maximum) candidate.fan_min_speed = maximum;
    uint8_t result = params_commit(candidate, maximum, false, NULL);
    xSemaphoreGive(g_commit_mutex);

    if (result == PARAM_STATUS_OK && candidate.fan_min_speed != previous_minimum) {
        ESP_LOGW(TAG, "Maximum saved at %u%%, working minimum reduced from %ld%% to %ld%%",
                 (unsigned)maximum, (long)previous_minimum, (long)candidate.fan_min_speed);
    }
    return result;
}

bool params_factory_reset(void) {
    xSemaphoreTake(g_commit_mutex, portMAX_DELAY);
    __atomic_add_fetch(&g_reset_revision, 1, __ATOMIC_RELEASE);
    if (!output_limit_begin_config_write()) {
        xSemaphoreGive(g_commit_mutex);
        return false;
    }
    params_t defaults = params_defaults();

    bool saved = params_save_to_nvs(&defaults, PARAMS_OUTPUT_MAX_DEFAULT);

    state_lock();
    if (saved) {
        g_config = (params_config_t){.params = defaults, .maximum = PARAMS_OUTPUT_MAX_DEFAULT};
    }
    g_pending_owner = BLE_HS_CONN_HANDLE_NONE;
    g_pending_mask = 0;
    set_last_status_locked(saved ? PARAM_STATUS_OK : PARAM_STATUS_STORAGE_ERROR, PARAM_FIELD_NONE);
    state_unlock();

    if (saved) {
        device_status_set_error_flag(DEVICE_ERROR_CONFIG_STORAGE, false);
        ESP_LOGW(TAG, "Parameters reset to defaults");
    }
    output_limit_end_config_write();
    xSemaphoreGive(g_commit_mutex);
    return saved;
}
