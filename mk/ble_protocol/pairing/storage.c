#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "crypto.h"
#include "state.h"
#include "storage.h"

#define NVS_NS              "pair"
#define NVS_KEY_STORE       "clients"
#define NVS_KEY_OLD_VER     "ver"
#define NVS_KEY_OLD_HOSTID  "hostid"
#define NVS_KEY_OLD_K       "keyK"
#define NVS_KEY_OLD_FORCE   "force_pair"
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

static bool trust_store_read_old_single(nvs_handle_t h) {
    uint32_t ver = 0;
    if (nvs_get_u32(h, NVS_KEY_OLD_VER, &ver) != ESP_OK || ver != 1) {
        return false;
    }

    uint8_t host_id_hash[32];
    uint8_t key[32];
    size_t len = sizeof(host_id_hash);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_OLD_HOSTID, host_id_hash, &len);
    if (err != ESP_OK || len != sizeof(host_id_hash)) {
        return false;
    }

    len = sizeof(key);
    err = nvs_get_blob(h, NVS_KEY_OLD_K, key, &len);
    if (err != ESP_OK || len != sizeof(key)) {
        return false;
    }

    trust_store_reset_ram();
    g_store.slots[0].used = 1;
    memcpy(g_store.slots[0].host_id_hash, host_id_hash, sizeof(host_id_hash));
    memcpy(g_store.slots[0].key, key, sizeof(key));
    g_store.next_slot = 1;
    return true;
}

static void trust_store_cleanup_legacy_keys(nvs_handle_t h) {
    nvs_erase_key(h, NVS_KEY_OLD_VER);
    nvs_erase_key(h, NVS_KEY_OLD_HOSTID);
    nvs_erase_key(h, NVS_KEY_OLD_K);
    nvs_erase_key(h, NVS_KEY_OLD_FORCE);
}

static void trust_store_save_ram(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for trust save");
        return;
    }

    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_STORE, &g_store, sizeof(g_store)));
    trust_store_cleanup_legacy_keys(h);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
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

    if (trust_store_read_old_single(h)) {
        nvs_close(h);
        trust_store_save_ram();
        fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
        ESP_LOGW(TAG, "Migrated legacy single-client trust store");
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

    for (size_t i = 0; i < TRUST_MAX_CLIENTS; i++) {
        const trust_slot_t *slot = &g_store.slots[i];
        uint8_t expect[32];
        if (slot->used != 1) {
            continue;
        }
        if (hmac_sha256(slot->key, sizeof(slot->key), msg, sizeof(msg), expect) != 0) {
            continue;
        }
        if (memcmp(expect, proof, sizeof(expect)) != 0) {
            continue;
        }
        if (out_host_id_hash != NULL) {
            memcpy(out_host_id_hash, slot->host_id_hash, 32);
        }
        return true;
    }

    return false;
}

void trust_store_upsert(const uint8_t host_id_hash[32], const uint8_t k[32]) {
    uint8_t count_before = trust_store_count_ram();

    for (size_t i = 0; i < TRUST_MAX_CLIENTS; i++) {
        trust_slot_t *slot = &g_store.slots[i];
        if (slot->used != 1) {
            continue;
        }
        if (memcmp(slot->host_id_hash, host_id_hash, 32) != 0) {
            continue;
        }
        memcpy(slot->key, k, 32);
        trust_store_save_ram();
        if (count_before == 0) {
            fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
        }
        return;
    }

    uint8_t slot_index = g_store.next_slot % TRUST_MAX_CLIENTS;
    trust_slot_t *slot = &g_store.slots[slot_index];
    slot->used = 1;
    memcpy(slot->host_id_hash, host_id_hash, 32);
    memcpy(slot->key, k, 32);
    g_store.next_slot = (uint8_t)((slot_index + 1) % TRUST_MAX_CLIENTS);

    trust_store_save_ram();
    fsm_dispatch(FSM_EVT_TRUST_LOADED, BLE_HS_CONN_HANDLE_NONE);
    ESP_LOGI(TAG, "Trusted client stored in slot %u (count=%u)",
             (unsigned)slot_index, (unsigned)trust_store_count_ram());
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
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_STORE);
        trust_store_cleanup_legacy_keys(h);
        nvs_commit(h);
        nvs_close(h);
    }

    trust_store_reset_ram();
    fsm_dispatch(FSM_EVT_TRUST_RESET, BLE_HS_CONN_HANDLE_NONE);
    ESP_LOGW(TAG, "Trust reset");
}
