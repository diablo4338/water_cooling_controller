#include <string.h>

#include "esp_log.h"

#include "host/ble_att.h"
#include "host/ble_gatt.h"

#include "crypto.h"
#include "ecdh.h"
#include "gatt.h"
#include "state.h"
#include "storage.h"
#include "uuid.h"

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

struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = pair_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = main_chrs },
    { 0 }
};

// ====== UUID parse + patch ======
static void parse_uuid_or_abort(const char *s, ble_uuid_any_t *out) {
    int rc = ble_uuid_from_str(out, s);
    if (rc != 0) {
        ESP_LOGE(TAG, "UUID parse failed: %s (rc=%d)", s, rc);
        abort();
    }
}

void gatt_init_uuids_and_services(void) {
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
