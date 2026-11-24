#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "crypto.h"
#include "state.h"
#include "storage.h"

#define NVS_NS              "pair"
#define NVS_KEY_STORE       "clients"
#define NVS_VER_VALUE       2

typedef struct {
    uint8_t used;
    uint8_t reserved[3];
    uint8_t host_id_hash[32];
    uint8_t key[32];
} trust_slot_t;

typedef struct {
    uint32_t version;
    uint8_t next_slot;
    uint8_t reserved[3];
    trust_slot_t slots[TRUST_MAX_CLIENTS];
} trust_store_t;

static trust_store_t g_store;
static StaticSemaphore_t s_store_mutex_storage;
static SemaphoreHandle_t s_store_mutex;

void trust_store_init(void) {
    s_store_mutex = xSemaphoreCreateMutexStatic(&s_store_mutex_storage);
}

static void trust_store_reset_ram(void) {
    memset(&g_store, 0, sizeof(g_store));
    g_store.version = NVS_VER_VALUE;
}

static uint8_t trust_store_count_ram(void) {
    uint8_t count = 0;
    for (size_t i = 0; i < TRUST_MAX_CLIENTS; i++) {
        if (g_store.slots[i].used == 1) {
            count++;
        }
    }
    return count;
}

static esp_err_t trust_store_save_ram(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_STORE, &g_store, sizeof(g_store));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}

void nvs_load_or_empty(void) {
    trust_store_reset_ram();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No trust store in NVS yet");
        return;
    }

    size_t len = sizeof(g_store);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_STORE, &g_store, &len);
    if (err == ESP_OK && len == sizeof(g_store) && g_store.version == NVS_VER_VALUE) {
        if (g_store.next_slot >= TRUST_MAX_CLIENTS) {
            g_store.next_slot = 0;
        }
        nvs_close(h);
        if (trust_store_count_ram() > 0) {
            fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
            ESP_LOGI(TAG, "Loaded %u trusted client(s) from NVS",
                     (unsigned)trust_store_count_ram());
        }
        return;
    }

    nvs_close(h);
    trust_store_reset_ram();
    ESP_LOGW(TAG, "Trust store missing or invalid; starting empty");
}

bool trust_store_match_auth(const uint8_t auth_nonce[16],
                            const uint8_t proof[32],
                            uint8_t out_host_id_hash[32]) {
    uint8_t msg[20];
    memcpy(msg, "auth", 4);
    memcpy(msg + 4, auth_nonce, 16);

    bool matched = false;
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    for (size_t i = 0; i < TRUST_MAX_CLIENTS; i++) {
        const trust_slot_t *slot = &g_store.slots[i];
        uint8_t expect[32];
        if (slot->used != 1) {
            continue;
        }
        if (pair_hmac_sha256(slot->key, sizeof(slot->key), msg, sizeof(msg), expect) != 0) {
            continue;
        }
        if (memcmp(expect, proof, sizeof(expect)) != 0) {
            continue;
        }
        if (out_host_id_hash != NULL) {
            memcpy(out_host_id_hash, slot->host_id_hash, 32);
        }
        matched = true;
        break;
    }

    xSemaphoreGive(s_store_mutex);
    return matched;
}

bool trust_store_upsert(const uint8_t host_id_hash[32], const uint8_t k[32]) {
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    uint8_t previous_next = g_store.next_slot;
    uint8_t slot_index = previous_next % TRUST_MAX_CLIENTS;
    bool existing = false;
    for (uint8_t i = 0; i < TRUST_MAX_CLIENTS; i++) {
        const trust_slot_t *slot = &g_store.slots[i];
        if (slot->used == 1 && memcmp(slot->host_id_hash, host_id_hash, 32) == 0) {
            slot_index = i;
            existing = true;
            break;
        }
    }

    trust_slot_t *slot = &g_store.slots[slot_index];
    trust_slot_t previous_slot = *slot;
    slot->used = 1;
    memcpy(slot->host_id_hash, host_id_hash, 32);
    memcpy(slot->key, k, 32);
    if (!existing) g_store.next_slot = (uint8_t)((slot_index + 1) % TRUST_MAX_CLIENTS);

    esp_err_t err = trust_store_save_ram();
    if (err == ESP_OK) {
        if (!existing) fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
    } else {
        *slot = previous_slot;
        g_store.next_slot = previous_next;
        ESP_LOGW(TAG, "Trust store save failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_store_mutex);
    return err == ESP_OK;
}

uint8_t trust_store_count(void) {
    return trust_store_count_ram();
}

uint8_t trust_store_next_slot(void) {
    return g_store.next_slot % TRUST_MAX_CLIENTS;
}

bool trust_store_get_entry(uint8_t slot, uint8_t out_host_id_hash[32], uint8_t out_k[32]) {
    if (slot >= TRUST_MAX_CLIENTS || g_store.slots[slot].used != 1) {
        return false;
    }
    if (out_host_id_hash != NULL) {
        memcpy(out_host_id_hash, g_store.slots[slot].host_id_hash, 32);
    }
    if (out_k != NULL) {
        memcpy(out_k, g_store.slots[slot].key, 32);
    }
    return true;
}

void trust_reset(void) {
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY_STORE);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Persistent trust reset failed: %s", esp_err_to_name(err));
    }

    trust_store_reset_ram();
    fsm_dispatch(FSM_EVT_TRUST_RESET, BLE_HS_CONN_HANDLE_NONE);
    xSemaphoreGive(s_store_mutex);
    ESP_LOGW(TAG, "Trust reset");
}
