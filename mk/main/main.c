// main/main.c
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "host/ble_att.h"

#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"

static const char *TAG = "pair-stack";

#define PAIR_BTN_GPIO 7

// ====== UUID strings (canonical, MUST match Python 1:1) ======
static const char *UUID_PAIR_SVC_STR       = "8fdd08d6-2a9e-4d5a-9f44-9f58b3a9d3c1";
static const char *UUID_MAIN_SVC_STR       = "3d1a4b35-9707-43e6-bf3e-2e2f7b561d82";

static const char *UUID_PAIR_DEV_NONCE_STR = "0b46b3cf-7e3b-44a3-8f39-4af2a8c9a1ee";
static const char *UUID_PAIR_DEV_PUB_STR   = "91c66f66-5c92-4c4d-86bf-6d2c58b6f0d7";
static const char *UUID_PAIR_HOST_PUB_STR  = "c9c8f69a-1f49-4ea0-a0a2-3c0d0a69e9d4";
static const char *UUID_PAIR_CONFIRM_STR   = "f5ee9c0b-96ae-4dc0-9b46-5f6f7f2ad2bf";
static const char *UUID_PAIR_FINISH_STR    = "a4c8e2c1-1c7b-4b06-a59f-4b5f8a2a8b3c";

static const char *UUID_AUTH_NONCE_STR     = "f1d1f9b6-8c92-47f6-a2f5-5b0a77d2e3a9";
static const char *UUID_AUTH_PROOF_STR     = "74cde77a-7f14-4e6e-b7f5-92ef0c3ad7e4";

// Mock data stream (MAIN)
static const char *UUID_MAIN_DATA_STR      = "a9b66c3d-3a6e-4b75-8b67-1dfbdb2a7e11";

// ====== Parsed UUIDs ======
static ble_uuid_any_t UUID_PAIR_SVC;
static ble_uuid_any_t UUID_MAIN_SVC;

static ble_uuid_any_t UUID_PAIR_DEV_NONCE;
static ble_uuid_any_t UUID_PAIR_DEV_PUB;
static ble_uuid_any_t UUID_PAIR_HOST_PUB;
static ble_uuid_any_t UUID_PAIR_CONFIRM;
static ble_uuid_any_t UUID_PAIR_FINISH;

static ble_uuid_any_t UUID_AUTH_NONCE;
static ble_uuid_any_t UUID_AUTH_PROOF;

static ble_uuid_any_t UUID_MAIN_DATA;

// ====== State ======
static bool g_pairing_mode = false;
static bool g_paired = false;
static bool g_authed = false;

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  g_own_addr_type;

// Nonces
static uint8_t dev_nonce[16];
static uint8_t auth_nonce[16];

// ====== ECDH ======
static mbedtls_ecp_group ec_grp;
static mbedtls_mpi       ec_d;
static mbedtls_ecp_point ec_Q;

static uint8_t dev_pub65[65];
static uint8_t host_pub65[65];

static uint8_t K[32];
static uint8_t host_id_hash[32];

// ====== NVS ======
#define NVS_NS         "pair"
#define NVS_KEY_HOSTID "hostid"
#define NVS_KEY_K      "keyK"

// ====== Timers ======
static esp_timer_handle_t g_term_timer;
static esp_timer_handle_t g_data_timer;

// Notify state for MAIN_DATA
static bool g_data_notify_enabled = false;
static uint16_t g_data_attr_handle = 0;

// ====== Helpers ======
static void rand_bytes(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        uint32_t r = esp_random();
        size_t chunk = (n - i < 4) ? (n - i) : 4;
        memcpy(out + i, &r, chunk);
    }
}

static int my_rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    rand_bytes(out, len);
    return 0;
}

static int hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t msg_len,
                       uint8_t out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    if (mbedtls_md_hmac(md, key, key_len, msg, msg_len, out) != 0) return -1;
    return 0;
}

static int hkdf_sha256_32(const uint8_t *salt, size_t salt_len,
                          const uint8_t *ikm, size_t ikm_len,
                          const uint8_t *info, size_t info_len,
                          uint8_t out32[32]) {
    uint8_t prk[32];
    if (hmac_sha256(salt, salt_len, ikm, ikm_len, prk) != 0) return -1;

    uint8_t buf[128];
    if (info_len + 1 > sizeof(buf)) return -1;
    memcpy(buf, info, info_len);
    buf[info_len] = 0x01;

    if (hmac_sha256(prk, sizeof(prk), buf, info_len + 1, out32) != 0) return -1;
    return 0;
}

static int sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 0) != 0) { mbedtls_md_free(&ctx); return -1; }
    if (mbedtls_md_starts(&ctx) != 0) { mbedtls_md_free(&ctx); return -1; }
    if (mbedtls_md_update(&ctx, msg, msg_len) != 0) { mbedtls_md_free(&ctx); return -1; }
    if (mbedtls_md_finish(&ctx, out) != 0) { mbedtls_md_free(&ctx); return -1; }
    mbedtls_md_free(&ctx);
    return 0;
}

// ====== NVS ======
static void nvs_load_or_empty(void) {
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

static void nvs_save_trust(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_HOSTID, host_id_hash, sizeof(host_id_hash)));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_K, K, sizeof(K)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Saved trust to NVS");
}

static void trust_reset(void) {
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

// ====== Terminate timer ======
static void term_cb(void *arg) {
    (void)arg;
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "Terminating conn after finish (handle=%u)", (unsigned)g_conn_handle);
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

// ====== ECDH ======
static int ecdh_make_dev_keys(void) {
    mbedtls_ecp_group_free(&ec_grp);
    mbedtls_ecp_group_init(&ec_grp);

    mbedtls_mpi_free(&ec_d);
    mbedtls_mpi_init(&ec_d);

    mbedtls_ecp_point_free(&ec_Q);
    mbedtls_ecp_point_init(&ec_Q);

    int rc = mbedtls_ecp_group_load(&ec_grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) return rc;

    rc = mbedtls_ecdh_gen_public(&ec_grp, &ec_d, &ec_Q, my_rng, NULL);
    if (rc != 0) return rc;

    size_t olen = 0;
    uint8_t tmp[80];
    rc = mbedtls_ecp_point_write_binary(&ec_grp, &ec_Q,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, tmp, sizeof(tmp));
    if (rc != 0) return rc;
    if (olen != 65) return -1;

    memcpy(dev_pub65, tmp, 65);
    return 0;
}

static int ecdh_compute_shared_secret(const uint8_t host_pub[65], uint8_t out_secret32[32]) {
    mbedtls_ecp_point Qp;
    mbedtls_ecp_point_init(&Qp);

    int rc = mbedtls_ecp_point_read_binary(&ec_grp, &Qp, host_pub, 65);
    if (rc != 0) { mbedtls_ecp_point_free(&Qp); return rc; }

    mbedtls_mpi z;
    mbedtls_mpi_init(&z);

    rc = mbedtls_ecdh_compute_shared(&ec_grp, &z, &Qp, &ec_d, my_rng, NULL);
    if (rc != 0) {
        mbedtls_mpi_free(&z);
        mbedtls_ecp_point_free(&Qp);
        return rc;
    }

    rc = mbedtls_mpi_write_binary(&z, out_secret32, 32);

    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp);
    return rc;
}

// ====== GATT access callbacks ======
static int gatt_read_dev_nonce(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    os_mbuf_append(ctxt->om, dev_nonce, sizeof(dev_nonce));
    return 0;
}

static int gatt_read_dev_pub(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    os_mbuf_append(ctxt->om, dev_pub65, sizeof(dev_pub65));
    return 0;
}

static int gatt_write_host_pub(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 65) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    os_mbuf_copydata(ctxt->om, 0, 65, host_pub65);

    uint8_t secret[32];
    if (ecdh_compute_shared_secret(host_pub65, secret) != 0) return BLE_ATT_ERR_UNLIKELY;

    const uint8_t info[] = "PAIRv1";
    if (hkdf_sha256_32(dev_nonce, sizeof(dev_nonce),
                       secret, sizeof(secret),
                       info, sizeof(info) - 1, K) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (sha256(host_pub65, 65, host_id_hash) != 0) return BLE_ATT_ERR_UNLIKELY;

    ESP_LOGI(TAG, "Host pub received; K derived");
    return 0;
}

static int gatt_write_pair_confirm(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t got[32];
    os_mbuf_copydata(ctxt->om, 0, 32, got);

    uint8_t msg[64];
    size_t off = 0;
    memcpy(msg + off, "confirm", 7); off += 7;
    memcpy(msg + off, dev_nonce, sizeof(dev_nonce)); off += sizeof(dev_nonce);

    uint8_t expect[32];
    if (hmac_sha256(K, sizeof(K), msg, off, expect) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (memcmp(got, expect, 32) != 0) {
        ESP_LOGW(TAG, "PAIR_CONFIRM failed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    ESP_LOGI(TAG, "PAIR_CONFIRM ok");
    return 0;
}

static int gatt_write_pair_finish(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t b;
    os_mbuf_copydata(ctxt->om, 0, 1, &b);
    if (b != 0x01) return BLE_ATT_ERR_UNLIKELY;

    nvs_save_trust();
    g_paired = true;
    g_pairing_mode = false;

    esp_timer_stop(g_term_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(g_term_timer, 250 * 1000));

    ESP_LOGI(TAG, "Pairing finished; pairing mode off");
    return 0;
}

static int gatt_read_auth_nonce(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    os_mbuf_append(ctxt->om, auth_nonce, sizeof(auth_nonce));
    return 0;
}

static int gatt_write_auth_proof(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (!g_paired) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t got[32];
    os_mbuf_copydata(ctxt->om, 0, 32, got);

    uint8_t msg[64];
    size_t off = 0;
    memcpy(msg + off, "auth", 4); off += 4;
    memcpy(msg + off, auth_nonce, sizeof(auth_nonce)); off += sizeof(auth_nonce);

    uint8_t expect[32];
    if (hmac_sha256(K, sizeof(K), msg, off, expect) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (memcmp(got, expect, 32) != 0) {
        g_authed = false;
        ESP_LOGW(TAG, "AUTH failed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    g_authed = true;
    ESP_LOGI(TAG, "AUTH ok (authed=true)");
    return 0;
}

// ====== Mock DATA characteristic ======
static int gatt_read_main_data(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    uint8_t buf[16];
    rand_bytes(buf, sizeof(buf));
    os_mbuf_append(ctxt->om, buf, sizeof(buf));
    return 0;
}

static void data_timer_cb(void *arg) {
    (void)arg;

    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!g_data_notify_enabled) return;
    if (g_data_attr_handle == 0) return;

    uint8_t payload[20];
    rand_bytes(payload, sizeof(payload));

    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (!om) return;

    int rc = ble_gatts_notify_custom(g_conn_handle, g_data_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify_custom rc=%d", rc);
    }
}

// ====== GATT db (RAM) ======
static struct ble_gatt_chr_def pair_chrs[] = {
    { .uuid = NULL, .access_cb = gatt_read_dev_nonce,     .flags = BLE_GATT_CHR_F_READ  },
    { .uuid = NULL, .access_cb = gatt_read_dev_pub,       .flags = BLE_GATT_CHR_F_READ  },
    { .uuid = NULL, .access_cb = gatt_write_host_pub,     .flags = BLE_GATT_CHR_F_WRITE },
    { .uuid = NULL, .access_cb = gatt_write_pair_confirm, .flags = BLE_GATT_CHR_F_WRITE },
    {
        .uuid = NULL,
        .access_cb = gatt_write_pair_finish,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
    },
    { 0 }
};

static struct ble_gatt_chr_def main_chrs[] = {
    { .uuid = NULL, .access_cb = gatt_read_auth_nonce,  .flags = BLE_GATT_CHR_F_READ  },
    { .uuid = NULL, .access_cb = gatt_write_auth_proof, .flags = BLE_GATT_CHR_F_WRITE },

    {
        .uuid = NULL,
        .access_cb = gatt_read_main_data,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_data_attr_handle,
    },

    { 0 }
};

static struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = pair_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = main_chrs },
    { 0 }
};

// ====== Advertising / GAP ======
static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void) {
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    if (g_pairing_mode) {
        adv_fields.uuids128 = (ble_uuid128_t *)&UUID_PAIR_SVC.u128;
        adv_fields.num_uuids128 = 1;
        adv_fields.uuids128_is_complete = 1;
    } else {
        adv_fields.uuids128 = (ble_uuid128_t *)&UUID_MAIN_SVC.u128;
        adv_fields.num_uuids128 = 1;
        adv_fields.uuids128_is_complete = 1;
    }

    uint8_t adv_data[31];
    uint8_t adv_len = 0;

    int rc = ble_hs_adv_set_fields(&adv_fields, adv_data, &adv_len, sizeof(adv_data));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_adv_set_fields rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_set_data(adv_data, adv_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    const char *name = g_pairing_mode ? "sensor-pair" : "sensor";
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = (uint8_t)strlen(name);
    rsp_fields.name_is_complete = 1;

    uint8_t rsp_data[31];
    uint8_t rsp_len = 0;

    rc = ble_hs_adv_set_fields(&rsp_fields, rsp_data, &rsp_len, sizeof(rsp_data));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_adv_set_fields (rsp) rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_rsp_set_data(rsp_data, rsp_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started (%s)", g_pairing_mode ? "PAIR" : "MAIN");
}

static void stop_advertising(void) {
    ble_gap_adv_stop();
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_conn_handle = event->connect.conn_handle;
                g_authed = false;
                rand_bytes(auth_nonce, sizeof(auth_nonce));
                ESP_LOGI(TAG, "Connected (handle=%d)", g_conn_handle);

                esp_timer_stop(g_data_timer);
                ESP_ERROR_CHECK(esp_timer_start_periodic(g_data_timer, 1000 * 1000));
            } else {
                ESP_LOGI(TAG, "Connect failed; restarting adv");
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected; restarting adv");
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_authed = false;
            g_data_notify_enabled = false;
            esp_timer_stop(g_data_timer);
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_data_attr_handle) {
                g_data_notify_enabled = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "DATA notify: %s", g_data_notify_enabled ? "ON" : "OFF");
            }
            return 0;

        default:
            return 0;
    }
}

// ====== UUID parse + patch ======
static void parse_uuid_or_abort(const char *s, ble_uuid_any_t *out) {
    int rc = ble_uuid_from_str(out, s);
    if (rc != 0) {
        ESP_LOGE(TAG, "UUID parse failed: %s (rc=%d)", s, rc);
        abort();
    }
}

static void init_uuids_and_patch_gatt(void) {
    parse_uuid_or_abort(UUID_PAIR_SVC_STR, &UUID_PAIR_SVC);
    parse_uuid_or_abort(UUID_MAIN_SVC_STR, &UUID_MAIN_SVC);

    parse_uuid_or_abort(UUID_PAIR_DEV_NONCE_STR, &UUID_PAIR_DEV_NONCE);
    parse_uuid_or_abort(UUID_PAIR_DEV_PUB_STR,   &UUID_PAIR_DEV_PUB);
    parse_uuid_or_abort(UUID_PAIR_HOST_PUB_STR,  &UUID_PAIR_HOST_PUB);
    parse_uuid_or_abort(UUID_PAIR_CONFIRM_STR,   &UUID_PAIR_CONFIRM);
    parse_uuid_or_abort(UUID_PAIR_FINISH_STR,    &UUID_PAIR_FINISH);

    parse_uuid_or_abort(UUID_AUTH_NONCE_STR, &UUID_AUTH_NONCE);
    parse_uuid_or_abort(UUID_AUTH_PROOF_STR, &UUID_AUTH_PROOF);

    parse_uuid_or_abort(UUID_MAIN_DATA_STR, &UUID_MAIN_DATA);

    gatt_svcs[0].uuid = &UUID_PAIR_SVC.u;
    gatt_svcs[1].uuid = &UUID_MAIN_SVC.u;

    pair_chrs[0].uuid = &UUID_PAIR_DEV_NONCE.u;
    pair_chrs[1].uuid = &UUID_PAIR_DEV_PUB.u;
    pair_chrs[2].uuid = &UUID_PAIR_HOST_PUB.u;
    pair_chrs[3].uuid = &UUID_PAIR_CONFIRM.u;
    pair_chrs[4].uuid = &UUID_PAIR_FINISH.u;

    main_chrs[0].uuid = &UUID_AUTH_NONCE.u;
    main_chrs[1].uuid = &UUID_AUTH_PROOF.u;
    main_chrs[2].uuid = &UUID_MAIN_DATA.u;
}

// ====== Sync ======
static void ble_app_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }
    start_advertising();
}

// ====== Button task ======
static void button_task(void *p) {
    (void)p;

    int prev = 1;
    int64_t press_start = 0;

    while (1) {
        int v = gpio_get_level(PAIR_BTN_GPIO);

        if (prev == 1 && v == 0) {
            press_start = esp_timer_get_time();
        }

        if (prev == 0 && v == 1) {
            int64_t dur_ms = (esp_timer_get_time() - press_start) / 1000;

            if (dur_ms > 9000) {
                trust_reset();
            } else {
                g_pairing_mode = true;
                rand_bytes(dev_nonce, sizeof(dev_nonce));

                if (ecdh_make_dev_keys() != 0) {
                    ESP_LOGE(TAG, "ECDH keygen failed");
                } else {
                    stop_advertising();
                    start_advertising();
                    ESP_LOGW(TAG, "Pairing mode ON (60s)");
                }

                vTaskDelay(pdMS_TO_TICKS(60000));

                if (g_pairing_mode) {
                    g_pairing_mode = false;
                    stop_advertising();
                    start_advertising();
                    ESP_LOGW(TAG, "Pairing mode OFF (timeout)");
                }
            }
        }

        prev = v;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ====== NimBLE host task ======
static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_load_or_empty();

    mbedtls_ecp_group_init(&ec_grp);
    mbedtls_mpi_init(&ec_d);
    mbedtls_ecp_point_init(&ec_Q);

    esp_timer_create_args_t targs = {
        .callback = term_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "term"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &g_term_timer));

    esp_timer_create_args_t dargs = {
        .callback = data_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "data"
    };
    ESP_ERROR_CHECK(esp_timer_create(&dargs, &g_data_timer));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PAIR_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreate(button_task, "btn", 4096, NULL, 5, NULL);

    init_uuids_and_patch_gatt();

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set("sensor");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);

    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(host_task);
}