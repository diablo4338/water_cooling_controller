#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "pair_state.h"
#include "state.h"
#include "storage.h"

#define NVS_NS         "pair"
#define NVS_KEY_VER    "ver"
#define NVS_KEY_HOSTID "hostid"
#define NVS_KEY_K      "keyK"
#define NVS_KEY_FORCE  "force_pair"
#define NVS_VER_VALUE  1

void nvs_load_or_empty(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No NVS namespace yet");
        return;
    }

    uint32_t ver = 0;
    if (nvs_get_u32(h, NVS_KEY_VER, &ver) != ESP_OK || ver != NVS_VER_VALUE) {
        nvs_close(h);
        ESP_LOGW(TAG, "NVS version mismatch; clearing trust");
        trust_reset();
        return;
    }

    size_t len = sizeof(host_id_hash);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_HOSTID, host_id_hash, &len);
    if (err == ESP_OK && len == sizeof(host_id_hash)) {
        len = sizeof(K);
        err = nvs_get_blob(h, NVS_KEY_K, K, &len);
        if (err == ESP_OK && len == sizeof(K)) {
            fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
            ESP_LOGI(TAG, "Loaded trusted host + key from NVS");
        } else {
            nvs_close(h);
            ESP_LOGW(TAG, "Host ID without key; clearing trust");
            trust_reset();
            return;
        }
    }
    nvs_close(h);
}

bool nvs_load_trust_if_available(uint8_t out_host_id_hash[32], uint8_t out_k[32]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    uint32_t ver = 0;
    if (nvs_get_u32(h, NVS_KEY_VER, &ver) != ESP_OK || ver != NVS_VER_VALUE) {
        nvs_close(h);
        return false;
    }

    size_t len = 32;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_HOSTID, out_host_id_hash, &len);
    if (err != ESP_OK || len != 32) {
        nvs_close(h);
        return false;
    }

    len = 32;
    err = nvs_get_blob(h, NVS_KEY_K, out_k, &len);
    nvs_close(h);
    return (err == ESP_OK && len == 32);
}

void nvs_save_trust(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_u32(h, NVS_KEY_VER, NVS_VER_VALUE));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_HOSTID, host_id_hash, sizeof(host_id_hash)));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_K, K, sizeof(K)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Saved trust to NVS");
}

void trust_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_VER);
        nvs_erase_key(h, NVS_KEY_HOSTID);
        nvs_erase_key(h, NVS_KEY_K);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(host_id_hash, 0, sizeof(host_id_hash));
    memset(K, 0, sizeof(K));
    fsm_dispatch(FSM_EVT_TRUST_RESET, BLE_HS_CONN_HANDLE_NONE);
    esp_timer_stop(g_pair_timer);
    pair_state_full_reset();
    ESP_LOGW(TAG, "Trust reset");
}

bool nvs_is_pair_forced(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_FORCE, &v);
    nvs_close(h);
    return (err == ESP_OK && v == 1);
}

void nvs_set_pair_forced(bool enabled) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (enabled) {
        nvs_set_u8(h, NVS_KEY_FORCE, 1);
    } else {
        nvs_erase_key(h, NVS_KEY_FORCE);
    }
    nvs_commit(h);
    nvs_close(h);
}
