#include "params.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "state.h"

#define PARAMS_NS "params"
#define PARAMS_KEY_VER "ver"
#define PARAMS_KEY_BLOB "blob"
#define PARAMS_VER_VALUE 2

typedef struct {
    uint8_t status;
    uint8_t field;
} params_status_t;

static params_t g_current = {
    .target_temp_c = 25.0f,
    .fan_min_rpm = 1200.0f,
    .alarm_delta_c = 5.0f,
    .fan_min_speed = 0,
    .fan_control_type = PARAM_FAN_CONTROL_DC,
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
    memcpy(&out->target_temp_c, data + 2, 4);
    memcpy(&out->fan_min_rpm, data + 6, 4);
    memcpy(&out->alarm_delta_c, data + 10, 4);
    memcpy(&out->fan_min_speed, data + 14, 4);
    out->fan_control_type = data[18];
    return true;
}

static void params_encode_payload(const params_t *params, uint8_t mask, uint8_t *out) {
    out[0] = PARAMS_VERSION;
    out[1] = mask & PARAM_MASK_ALL;
    memcpy(out + 2, &params->target_temp_c, 4);
    memcpy(out + 6, &params->fan_min_rpm, 4);
    memcpy(out + 10, &params->alarm_delta_c, 4);
    memcpy(out + 14, &params->fan_min_speed, 4);
    out[18] = params->fan_control_type;
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
        g_current.target_temp_c = 25.0f;
        g_current.fan_min_rpm = 1200.0f;
        g_current.alarm_delta_c = 5.0f;
        g_current.fan_min_speed = 0;
        g_current.fan_control_type = PARAM_FAN_CONTROL_DC;
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
    if (!isfinite(params->target_temp_c) ||
        params->target_temp_c < 10.0f || params->target_temp_c > 90.0f) {
        if (field) *field = PARAM_FIELD_TARGET_TEMP;
        return false;
    }
    if (!isfinite(params->fan_min_rpm) ||
        params->fan_min_rpm < 300.0f || params->fan_min_rpm > 5000.0f) {
        if (field) *field = PARAM_FIELD_FAN_MIN_RPM;
        return false;
    }
    if (!isfinite(params->alarm_delta_c) ||
        params->alarm_delta_c < 0.5f || params->alarm_delta_c > 30.0f) {
        if (field) *field = PARAM_FIELD_ALARM_DELTA;
        return false;
    }
    if (params->fan_min_speed < 0 || params->fan_min_speed > 100) {
        if (field) *field = PARAM_FIELD_FAN_MIN_SPEED;
        return false;
    }
    if (params->fan_control_type != PARAM_FAN_CONTROL_DC &&
        params->fan_control_type != PARAM_FAN_CONTROL_PWM) {
        if (field) *field = PARAM_FIELD_FAN_CONTROL_TYPE;
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
        if (mask & PARAM_MASK_TARGET_TEMP) candidate.target_temp_c = pending.target_temp_c;
        if (mask & PARAM_MASK_FAN_MIN_RPM) candidate.fan_min_rpm = pending.fan_min_rpm;
        if (mask & PARAM_MASK_ALARM_DELTA) candidate.alarm_delta_c = pending.alarm_delta_c;
        if (mask & PARAM_MASK_FAN_MIN_SPEED) candidate.fan_min_speed = pending.fan_min_speed;
        if (mask & PARAM_MASK_FAN_CONTROL_TYPE) candidate.fan_control_type = pending.fan_control_type;
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
