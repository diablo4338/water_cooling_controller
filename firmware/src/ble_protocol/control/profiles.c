#include "profiles.h"

#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"

#include "params.h"
#include "state.h"

#define PROFILES_NS "profiles"
#define PROFILES_KEY_LEN 3
#define PROFILE_METADATA_HEADER_LEN 3

static bool profile_key(uint8_t slot, char key[PROFILES_KEY_LEN]) {
    if (slot < 1 || slot > PROFILE_COUNT) {
        return false;
    }
    snprintf(key, PROFILES_KEY_LEN, "p%u", (unsigned)slot);
    return true;
}

static bool profile_metadata_key(uint8_t slot, char key[PROFILES_KEY_LEN]) {
    if (slot < 1 || slot > PROFILE_COUNT) {
        return false;
    }
    snprintf(key, PROFILES_KEY_LEN, "m%u", (unsigned)slot);
    return true;
}

bool profiles_read_mask(uint8_t *mask) {
    if (mask == NULL) {
        return false;
    }
    *mask = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILES_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profiles NVS open failed: %d", err);
        return false;
    }
    for (uint8_t slot = 1; slot <= PROFILE_COUNT; slot++) {
        char key[PROFILES_KEY_LEN];
        size_t len = 0;
        if (profile_key(slot, key) &&
            nvs_get_blob(handle, key, NULL, &len) == ESP_OK &&
            len == PARAMS_PAYLOAD_LEN) {
            *mask |= (uint8_t)(1U << (slot - 1));
        }
    }
    nvs_close(handle);
    return true;
}

bool profiles_save_current(uint8_t slot) {
    char key[PROFILES_KEY_LEN];
    if (!profile_key(slot, key)) {
        return false;
    }
    uint8_t payload[PARAMS_PAYLOAD_LEN];
    if (!params_get_current_payload(payload, sizeof(payload))) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILES_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profiles NVS open failed: %d", err);
        return false;
    }
    err = nvs_set_blob(handle, key, payload, sizeof(payload));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profile %u save failed: %d", (unsigned)slot, err);
        return false;
    }
    return true;
}

static uint8_t g_apply_result[PROFILE_RESULT_PAYLOAD_LEN] = {PROFILE_RESULT_VERSION};

void profiles_get_result_payload(uint8_t out[PROFILE_RESULT_PAYLOAD_LEN]) {
    state_lock();
    memcpy(out, g_apply_result, sizeof(g_apply_result));
    state_unlock();
}

bool profiles_apply(uint8_t slot) {
    char key[PROFILES_KEY_LEN];
    if (!profile_key(slot, key)) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILES_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t payload[PARAMS_PAYLOAD_LEN];
    size_t len = sizeof(payload);
    err = nvs_get_blob(handle, key, payload, &len);
    nvs_close(handle);
    params_profile_result_t result;
    if (err != ESP_OK || len != sizeof(payload) ||
        !params_apply_profile_payload(payload, len, &result)) return false;

    state_lock();
    g_apply_result[1] = slot;
    g_apply_result[2] = result.status;
    g_apply_result[3] = (uint8_t)result.applied_mask;
    g_apply_result[4] = (uint8_t)(result.applied_mask >> 8);
    memcpy(g_apply_result + 5, result.skipped, sizeof(result.skipped));
    state_unlock();
    return true;
}

bool profiles_clear(uint8_t slot) {
    char key[PROFILES_KEY_LEN];
    if (!profile_key(slot, key)) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILES_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return true;
        }
        ESP_LOGW(TAG, "Profiles NVS open failed: %d", err);
        return false;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profile %u clear failed: %d", (unsigned)slot, err);
        return false;
    }
    return true;
}

bool profiles_read_metadata(uint8_t slot, uint8_t *out, size_t capacity, size_t *len) {
    char key[PROFILES_KEY_LEN];
    if (!profile_metadata_key(slot, key) || out == NULL || len == NULL ||
        capacity < PROFILE_METADATA_HEADER_LEN) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILES_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = PROFILE_METADATA_VERSION;
        out[1] = 0;
        out[2] = 0;
        *len = PROFILE_METADATA_HEADER_LEN;
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profiles NVS open failed: %d", err);
        return false;
    }

    size_t stored_len = capacity;
    err = nvs_get_blob(handle, key, out, &stored_len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = PROFILE_METADATA_VERSION;
        out[1] = 0;
        out[2] = 0;
        *len = PROFILE_METADATA_HEADER_LEN;
        return true;
    }
    if (err != ESP_OK || stored_len < PROFILE_METADATA_HEADER_LEN ||
        stored_len > capacity || out[0] != PROFILE_METADATA_VERSION ||
        (size_t)out[1] + out[2] + PROFILE_METADATA_HEADER_LEN != stored_len) {
        ESP_LOGW(TAG, "Profile %u metadata read failed: %d", (unsigned)slot, err);
        return false;
    }
    *len = stored_len;
    return true;
}

bool profiles_write_metadata(uint8_t slot, const uint8_t *data, size_t len) {
    char key[PROFILES_KEY_LEN];
    if (!profile_metadata_key(slot, key) || data == NULL ||
        len < PROFILE_METADATA_HEADER_LEN || len > PROFILE_METADATA_MAX_LEN ||
        data[0] != PROFILE_METADATA_VERSION ||
        data[1] > PROFILE_TITLE_MAX_LEN ||
        data[2] > PROFILE_DESCRIPTION_MAX_LEN ||
        (size_t)data[1] + data[2] + PROFILE_METADATA_HEADER_LEN != len) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILES_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profiles NVS open failed: %d", err);
        return false;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profile %u metadata save failed: %d", (unsigned)slot, err);
        return false;
    }
    return true;
}
