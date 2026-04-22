#include "params.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "state.h"

#define PARAMS_NS "params"
#define PARAMS_KEY_VER "ver"
#define PARAMS_KEY_BLOB "blob"
#define PARAMS_VER_VALUE 4

typedef struct {
    uint8_t status;
    uint8_t field;
} params_status_t;

static params_t g_current = {
    .fan_min_speed = 10,
    .fan_control_type = PARAM_FAN_CONTROL_DC,
    .fan_max_temp = 45,
    .fan_off_delta = 2,
    .fan_start_temp = 35,
    .fan_mode = PARAM_FAN_MODE_CONTINUOUS,
    .fan_monitoring_enabled = 1,
};
static params_t g_pending;
static uint8_t g_pending_mask = 0;
static bool g_pending_valid = false;
static params_status_t g_last_status = {PARAM_STATUS_OK, PARAM_FIELD_NONE};
static params_t g_cache;
static uint32_t g_cache_seq = 0;
static bool g_cache_valid = false;

static void params_cache_write(const params_t *params) {
    __atomic_fetch_add(&g_cache_seq, 1U, __ATOMIC_ACQ_REL);
    g_cache = *params;
    __atomic_store_n(&g_cache_valid, true, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_cache_seq, 1U, __ATOMIC_ACQ_REL);
}

static bool params_load_from_nvs(params_t *out) {
    nvs_handle_t h;
    if (nvs_open(PARAMS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint32_t ver = 0;
    if (nvs_get_u32(h, PARAMS_KEY_VER, &ver) != ESP_OK || ver != PARAMS_VER_VALUE) {
        nvs_close(h);
        return false;
    }
    size_t len = sizeof(params_t);
    esp_err_t err = nvs_get_blob(h, PARAMS_KEY_BLOB, out, &len);
    nvs_close(h);
    return (err == ESP_OK && len == sizeof(params_t));
}

static void params_save_to_nvs(const params_t *params) {
    nvs_handle_t h;
    if (nvs_open(PARAMS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Params NVS open failed");
        return;
    }
    esp_err_t err = nvs_set_u32(h, PARAMS_KEY_VER, PARAMS_VER_VALUE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Params NVS set ver failed: %d", err);
        nvs_close(h);
        return;
    }
    err = nvs_set_blob(h, PARAMS_KEY_BLOB, params, sizeof(params_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Params NVS set blob failed: %d", err);
        nvs_close(h);
        return;
    }
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Params NVS commit failed: %d", err);
    }
    nvs_close(h);
}

static bool params_decode_payload(const uint8_t *data, size_t len, params_t *out, uint8_t *mask) {
    if (len != PARAMS_PAYLOAD_LEN) return false;
    if (data[0] != PARAMS_VERSION) return false;
    if (out == NULL || mask == NULL) return false;

    *mask = data[1] & PARAM_MASK_ALL;
    memcpy(&out->fan_min_speed, data + 2, 4);
    out->fan_control_type = data[6];
    memcpy(&out->fan_max_temp, data + 7, 4);
    memcpy(&out->fan_off_delta, data + 11, 4);
    memcpy(&out->fan_start_temp, data + 15, 4);
    out->fan_mode = data[19];
    out->fan_monitoring_enabled = data[20];
    return true;
}

static void params_encode_payload(const params_t *params, uint8_t mask, uint8_t *out) {
    out[0] = PARAMS_VERSION;
    out[1] = mask & PARAM_MASK_ALL;
    memcpy(out + 2, &params->fan_min_speed, 4);
    out[6] = params->fan_control_type;
    memcpy(out + 7, &params->fan_max_temp, 4);
    memcpy(out + 11, &params->fan_off_delta, 4);
    memcpy(out + 15, &params->fan_start_temp, 4);
    out[19] = params->fan_mode;
    out[20] = params->fan_monitoring_enabled;
}

static void set_last_status_locked(uint8_t status, uint8_t field) {
    g_last_status.status = status;
    g_last_status.field = field;
}

void params_init(void) {
    params_t loaded;
    bool has_saved = params_load_from_nvs(&loaded);
    state_lock();
    if (has_saved) {
        g_current = loaded;
    } else {
        g_current.fan_min_speed = 10;
        g_current.fan_control_type = PARAM_FAN_CONTROL_DC;
        g_current.fan_max_temp = 45;
        g_current.fan_off_delta = 2;
        g_current.fan_start_temp = 35;
        g_current.fan_mode = PARAM_FAN_MODE_CONTINUOUS;
        g_current.fan_monitoring_enabled = 1;
    }
    g_pending_valid = false;
    g_pending_mask = 0;
    set_last_status_locked(PARAM_STATUS_OK, PARAM_FIELD_NONE);
    state_unlock();
    params_cache_write(&g_current);
}

bool params_read(params_t *out) {
    if (out == NULL) return false;
    state_lock();
    *out = g_current;
    state_unlock();
    return true;
}

bool params_write(const params_t *params, uint8_t mask) {
    if (params == NULL) return false;
    state_lock();
    g_pending = *params;
    g_pending_mask = mask & PARAM_MASK_ALL;
    g_pending_valid = true;
    state_unlock();
    return true;
}

bool params_cache_get(params_t *out) {
    if (out == NULL) return false;
    if (!__atomic_load_n(&g_cache_valid, __ATOMIC_ACQUIRE)) return false;
    while (1) {
        uint32_t seq1 = __atomic_load_n(&g_cache_seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1U) {
            continue;
        }
        params_t snap = g_cache;
        uint32_t seq2 = __atomic_load_n(&g_cache_seq, __ATOMIC_ACQUIRE);
        if (seq1 == seq2) {
            *out = snap;
            return true;
        }
    }
}

bool params_set_pending_payload(const uint8_t *data, size_t len) {
    params_t tmp;
    uint8_t mask = 0;
    if (!params_decode_payload(data, len, &tmp, &mask)) return false;
    return params_write(&tmp, mask);
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

static bool params_validate(const params_t *params, uint8_t *field) {
    if (params->fan_min_speed < 10 || params->fan_min_speed > 100) {
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
    if (params->fan_monitoring_enabled > 1) {
        if (field) *field = PARAM_FIELD_FAN_MONITORING_ENABLED;
        return false;
    }
    return true;
}

uint8_t params_apply(uint8_t *field_id) {
    params_t current;
    params_t pending;
    uint8_t mask = 0;
    bool pending_valid = false;

    state_lock();
    current = g_current;
    pending = g_pending;
    mask = g_pending_mask;
    pending_valid = g_pending_valid;
    state_unlock();

    params_t candidate = current;
    if (pending_valid) {
        if (mask & PARAM_MASK_FAN_MIN_SPEED) candidate.fan_min_speed = pending.fan_min_speed;
        if (mask & PARAM_MASK_FAN_CONTROL_TYPE) candidate.fan_control_type = pending.fan_control_type;
        if (mask & PARAM_MASK_FAN_MAX_TEMP) candidate.fan_max_temp = pending.fan_max_temp;
        if (mask & PARAM_MASK_FAN_OFF_DELTA) candidate.fan_off_delta = pending.fan_off_delta;
        if (mask & PARAM_MASK_FAN_START_TEMP) candidate.fan_start_temp = pending.fan_start_temp;
        if (mask & PARAM_MASK_FAN_MODE) candidate.fan_mode = pending.fan_mode;
        if (mask & PARAM_MASK_FAN_MONITORING_ENABLED) candidate.fan_monitoring_enabled = pending.fan_monitoring_enabled;
    }

    uint8_t field = PARAM_FIELD_NONE;
    if (!params_validate(&candidate, &field)) {
        state_lock();
        set_last_status_locked(PARAM_STATUS_INVALID, field);
        state_unlock();
        if (field_id) *field_id = field;
        return PARAM_STATUS_INVALID;
    }

    params_save_to_nvs(&candidate);

    state_lock();
    g_current = candidate;
    g_pending_valid = false;
    g_pending_mask = 0;
    set_last_status_locked(PARAM_STATUS_OK, PARAM_FIELD_NONE);
    state_unlock();
    params_cache_write(&candidate);

    if (field_id) *field_id = PARAM_FIELD_NONE;
    return PARAM_STATUS_OK;
}
