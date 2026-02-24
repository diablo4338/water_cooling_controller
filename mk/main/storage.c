#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "state.h"
#include "storage.h"

#define NVS_NS         "pair"
#define NVS_KEY_HOSTID "hostid"
#define NVS_KEY_K      "keyK"

void nvs_load_or_empty(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No NVS namespace yet");
        return;
    }

    size_t len = sizeof(host_id_hash);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_HOSTID, host_id_hash, &len);
    if (err == ESP_OK && len == sizeof(host_id_hash)) {
        len = sizeof(K);
        err = nvs_get_blob(h, NVS_KEY_K, K, &len);
        if (err == ESP_OK && len == sizeof(K)) {
            g_paired = true;
            ESP_LOGI(TAG, "Loaded trusted host + key from NVS");
        }
    }
    nvs_close(h);
}

void nvs_save_trust(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_HOSTID, host_id_hash, sizeof(host_id_hash)));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_K, K, sizeof(K)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Saved trust to NVS");
}

void trust_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_HOSTID);
        nvs_erase_key(h, NVS_KEY_K);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(host_id_hash, 0, sizeof(host_id_hash));
    memset(K, 0, sizeof(K));
    g_paired = false;
    g_authed = false;
    ESP_LOGW(TAG, "Trust reset");
}
