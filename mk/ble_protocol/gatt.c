#include <string.h>
#include <stdint.h>

#include "esp_log.h"

#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "access.h"
#include "crypto.h"
#include "ecdh.h"
#include "gatt.h"
#include "pair_state.h"
#include "pair_mode.h"
#include "state.h"
#include "storage.h"
#include "uuid.h"
#include "conn_guard.h"
#include "host_verify.h"
#include "metrics.h"
#include "metrics_ble.h"
#include "fan_control.h"
#include "fan_status_ble.h"
#include "params.h"

// ====== GATT access callbacks ======
static int gatt_read_dev_nonce(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pairing_conn_bind_or_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    uint8_t tmp[sizeof(dev_nonce)];
    state_lock();
    memcpy(tmp, dev_nonce, sizeof(dev_nonce));
    state_unlock();
    os_mbuf_append(ctxt->om, tmp, sizeof(tmp));
    return 0;
}

static int gatt_read_dev_pub(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pairing_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    uint8_t tmp[sizeof(dev_pub65)];
    state_lock();
    memcpy(tmp, dev_pub65, sizeof(dev_pub65));
    state_unlock();
    os_mbuf_append(ctxt->om, tmp, sizeof(tmp));
    return 0;
}

static int gatt_write_host_pub(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pair_state_can_host_pub()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pairing_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 65) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    os_mbuf_copydata(ctxt->om, 0, 65, host_pub65);

    uint8_t secret[32];
    if (ecdh_compute_shared_secret(host_pub65, secret) != 0) return BLE_ATT_ERR_UNLIKELY;

    const uint8_t info[] = "PAIRv1";
    uint8_t dev_nonce_local[sizeof(dev_nonce)];
    state_lock();
    memcpy(dev_nonce_local, dev_nonce, sizeof(dev_nonce));
    state_unlock();
    if (hkdf_sha256_32(dev_nonce_local, sizeof(dev_nonce_local),
                       secret, sizeof(secret),
                       info, sizeof(info) - 1, K) != 0) return BLE_ATT_ERR_UNLIKELY;

    host_verify_update(host_pub65);

    pair_state_set_host_pub_ok();
    ESP_LOGI(TAG, "Host pub received; K derived");
    return 0;
}

static int gatt_write_pair_confirm(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pair_state_can_confirm()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pairing_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t got[32];
    os_mbuf_copydata(ctxt->om, 0, 32, got);

    uint8_t msg[64];
    size_t off = 0;
    memcpy(msg + off, "confirm", 7); off += 7;
    uint8_t dev_nonce_local[sizeof(dev_nonce)];
    uint8_t K_local[sizeof(K)];
    state_lock();
    memcpy(dev_nonce_local, dev_nonce, sizeof(dev_nonce));
    memcpy(K_local, K, sizeof(K));
    state_unlock();
    memcpy(msg + off, dev_nonce_local, sizeof(dev_nonce_local)); off += sizeof(dev_nonce_local);

    uint8_t expect[32];
    if (hmac_sha256(K_local, sizeof(K_local), msg, off, expect) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (memcmp(got, expect, 32) != 0) {
        ESP_LOGW(TAG, "PAIR_CONFIRM failed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    pair_state_set_confirm_ok();
    ESP_LOGI(TAG, "PAIR_CONFIRM ok");
    return 0;
}

static int gatt_write_pair_finish(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pair_state_can_finish()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pairing_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t b;
    os_mbuf_copydata(ctxt->om, 0, 1, &b);
    if (b != 0x01) return BLE_ATT_ERR_UNLIKELY;

    nvs_save_trust();
    if (pair_mode_is_forced()) {
        pair_mode_clear_on_success(conn_handle);
    } else {
        fsm_dispatch(FSM_EVT_PAIR_FINISH, conn_handle);
    }
    pair_state_set_finish_ok();
    esp_timer_stop(g_pair_timer);

    esp_timer_stop(g_term_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(g_term_timer, 250 * 1000));

    ESP_LOGI(TAG, "Pairing finished; pairing mode off");
    return 0;
}

static int gatt_read_auth_nonce(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_auth_nonce()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!auth_conn_check_or_any(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    uint8_t tmp[sizeof(auth_nonce)];
    state_lock();
    memcpy(tmp, auth_nonce, sizeof(auth_nonce));
    state_unlock();
    os_mbuf_append(ctxt->om, tmp, sizeof(tmp));
    return 0;
}

static int gatt_write_auth_proof(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (!can_access_auth_nonce()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!auth_conn_check_or_any(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    bool already_authed = false;
    uint16_t authed_handle = BLE_HS_CONN_HANDLE_NONE;
    already_authed = fsm_is_authed();
    authed_handle = fsm_get_auth_conn_handle();
    if (already_authed) {
        if (authed_handle == conn_handle) {
            ESP_LOGI(TAG, "AUTH already ok; ignoring repeat proof");
            return 0;
        }
        ESP_LOGW(TAG, "AUTH proof from other handle while authed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t got[32];
    os_mbuf_copydata(ctxt->om, 0, 32, got);

    uint8_t msg[64];
    size_t off = 0;
    memcpy(msg + off, "auth", 4); off += 4;
    uint8_t auth_nonce_local[sizeof(auth_nonce)];
    uint8_t K_local[sizeof(K)];
    bool k_loaded = false;
    state_lock();
    memcpy(auth_nonce_local, auth_nonce, sizeof(auth_nonce));
    memcpy(K_local, K, sizeof(K));
    state_unlock();

    bool all_zero = true;
    for (size_t i = 0; i < sizeof(K_local); i++) {
        if (K_local[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) {
        bool paired = fsm_is_paired();
        if (paired) {
            uint8_t nvs_host_id_hash[32];
            uint8_t nvs_k[32];
            k_loaded = nvs_load_trust_if_available(nvs_host_id_hash, nvs_k);
            if (k_loaded) {
                ESP_LOGW(TAG, "K was empty; reloaded from NVS");
                state_lock();
                memcpy(host_id_hash, nvs_host_id_hash, sizeof(host_id_hash));
                memcpy(K_local, nvs_k, sizeof(K_local));
                memcpy(K, nvs_k, sizeof(K));
                state_unlock();
            }
        }
    }
    memcpy(msg + off, auth_nonce_local, sizeof(auth_nonce_local)); off += sizeof(auth_nonce_local);

    uint8_t expect[32];
    if (hmac_sha256(K_local, sizeof(K_local), msg, off, expect) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (memcmp(got, expect, 32) != 0) {
        fsm_dispatch(FSM_EVT_AUTH_FAILED, conn_handle);
        ESP_LOGW(TAG, "AUTH failed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    bool host_pub_present = false;
    state_lock();
    for (size_t i = 0; i < sizeof(host_pub65); i++) {
        if (host_pub65[i] != 0) { host_pub_present = true; break; }
    }
    state_unlock();
    if (host_pub_present && !host_verify_check()) {
        fsm_dispatch(FSM_EVT_AUTH_FAILED, conn_handle);
        ESP_LOGW(TAG, "AUTH host mismatch");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    fsm_dispatch(FSM_EVT_AUTH_OK, conn_handle);
    state_lock();
    memset(auth_nonce, 0, sizeof(auth_nonce));
    state_unlock();
    ESP_LOGI(TAG, "AUTH ok (authed=true)");
    return 0;
}

// ====== Metrics ======
static int gatt_read_temp_single(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t channel = (uint8_t)(uintptr_t)arg;
    float temp = metrics_get_temp(channel);
    os_mbuf_append(ctxt->om, &temp, sizeof(temp));
    return 0;
}

static int gatt_read_fan_speed(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    float rpm = metrics_get_fan_speed_rpm();
    os_mbuf_append(ctxt->om, &rpm, sizeof(rpm));
    return 0;
}

// ====== Config params ======
static int gatt_read_params(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t payload[PARAMS_PAYLOAD_LEN];
    if (!params_get_current_payload(payload, sizeof(payload))) return BLE_ATT_ERR_UNLIKELY;
    os_mbuf_append(ctxt->om, payload, sizeof(payload));
    return 0;
}

static int gatt_write_params(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (fan_control_is_calibrating()) {
        params_set_last_status(PARAM_STATUS_BUSY, PARAM_FIELD_NONE);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != PARAMS_PAYLOAD_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[PARAMS_PAYLOAD_LEN];
    os_mbuf_copydata(ctxt->om, 0, sizeof(buf), buf);
    if (!params_set_pending_payload(buf, sizeof(buf))) return BLE_ATT_ERR_UNLIKELY;
    return 0;
}

static int gatt_access_params(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return gatt_read_params(conn_handle, attr_handle, ctxt, arg);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return gatt_write_params(conn_handle, attr_handle, ctxt, arg);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static uint16_t g_params_status_attr_handle = 0;

static int gatt_access_params_status(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t payload[PARAMS_STATUS_LEN];
        params_get_last_status_payload(payload, sizeof(payload));
        os_mbuf_append(ctxt->om, payload, sizeof(payload));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t cmd = 0;
        os_mbuf_copydata(ctxt->om, 0, 1, &cmd);
        if (cmd != 0x01) return BLE_ATT_ERR_UNLIKELY;

        if (fan_control_is_calibrating()) {
            params_set_last_status(PARAM_STATUS_BUSY, PARAM_FIELD_NONE);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        params_apply(NULL);

        if (g_params_status_attr_handle != 0) {
            uint8_t payload[PARAMS_STATUS_LEN];
            params_get_last_status_payload(payload, sizeof(payload));
            struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
            if (om) {
                int rc = ble_gatts_notify_custom(conn_handle, g_params_status_attr_handle, om);
                if (rc != 0) {
                    ESP_LOGW(TAG, "notify params status rc=%d", rc);
                }
            }
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_read_fan_status(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t payload[FAN_STATUS_PAYLOAD_LEN];
    fan_control_get_status_payload(payload, sizeof(payload));
    os_mbuf_append(ctxt->om, payload, sizeof(payload));
    return 0;
}

static int gatt_access_fan_calibrate(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!auth_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t active = fan_control_is_calibrating() ? 1u : 0u;
        os_mbuf_append(ctxt->om, &active, sizeof(active));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t cmd = 0;
        os_mbuf_copydata(ctxt->om, 0, 1, &cmd);
        if (cmd != 0x01) return BLE_ATT_ERR_UNLIKELY;
        if (!fan_control_start_calibration()) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
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
    { 0 }
};

static struct ble_gatt_chr_def config_chrs[] = {
    {
        .uuid = NULL,
        .access_cb = gatt_access_params,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_params_status,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_params_status_attr_handle,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_fan_status,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_status_attr_handle,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_fan_calibrate,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    { 0 }
};

static struct ble_gatt_chr_def metrics_chrs[] = {
    {
        .uuid = NULL,
        .access_cb = gatt_read_temp_single,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_temp_attr_handles[0],
        .arg = (void *)(uintptr_t)0,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_temp_single,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_temp_attr_handles[1],
        .arg = (void *)(uintptr_t)1,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_temp_single,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_temp_attr_handles[2],
        .arg = (void *)(uintptr_t)2,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_temp_single,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_temp_attr_handles[3],
        .arg = (void *)(uintptr_t)3,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_fan_speed,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_attr_handle,
        .arg = NULL,
    },
    { 0 }
};

struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = pair_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = main_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = config_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = metrics_chrs },
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
    parse_uuid_or_abort(UUID_METRICS_SVC_STR, &UUID_METRICS_SVC);
    parse_uuid_or_abort(UUID_CONFIG_SVC_STR, &UUID_CONFIG_SVC);

    parse_uuid_or_abort(UUID_PAIR_DEV_NONCE_STR, &UUID_PAIR_DEV_NONCE);
    parse_uuid_or_abort(UUID_PAIR_DEV_PUB_STR,   &UUID_PAIR_DEV_PUB);
    parse_uuid_or_abort(UUID_PAIR_HOST_PUB_STR,  &UUID_PAIR_HOST_PUB);
    parse_uuid_or_abort(UUID_PAIR_CONFIRM_STR,   &UUID_PAIR_CONFIRM);
    parse_uuid_or_abort(UUID_PAIR_FINISH_STR,    &UUID_PAIR_FINISH);

    parse_uuid_or_abort(UUID_AUTH_NONCE_STR, &UUID_AUTH_NONCE);
    parse_uuid_or_abort(UUID_AUTH_PROOF_STR, &UUID_AUTH_PROOF);

    parse_uuid_or_abort(UUID_CONFIG_PARAMS_STR, &UUID_CONFIG_PARAMS);
    parse_uuid_or_abort(UUID_CONFIG_STATUS_STR, &UUID_CONFIG_STATUS);
    parse_uuid_or_abort(UUID_CONFIG_FAN_STATUS_STR, &UUID_CONFIG_FAN_STATUS);
    parse_uuid_or_abort(UUID_CONFIG_FAN_CALIBRATE_STR, &UUID_CONFIG_FAN_CALIBRATE);
    
    parse_uuid_or_abort(UUID_TEMP0_VALUE_STR, &UUID_TEMP0_VALUE);
    parse_uuid_or_abort(UUID_TEMP1_VALUE_STR, &UUID_TEMP1_VALUE);
    parse_uuid_or_abort(UUID_TEMP2_VALUE_STR, &UUID_TEMP2_VALUE);
    parse_uuid_or_abort(UUID_TEMP3_VALUE_STR, &UUID_TEMP3_VALUE);
    parse_uuid_or_abort(UUID_FAN_SPEED_VALUE_STR, &UUID_FAN_SPEED_VALUE);

    gatt_svcs[0].uuid = &UUID_PAIR_SVC.u;
    gatt_svcs[1].uuid = &UUID_MAIN_SVC.u;
    gatt_svcs[2].uuid = &UUID_CONFIG_SVC.u;
    gatt_svcs[3].uuid = &UUID_METRICS_SVC.u;

    pair_chrs[0].uuid = &UUID_PAIR_DEV_NONCE.u;
    pair_chrs[1].uuid = &UUID_PAIR_DEV_PUB.u;
    pair_chrs[2].uuid = &UUID_PAIR_HOST_PUB.u;
    pair_chrs[3].uuid = &UUID_PAIR_CONFIRM.u;
    pair_chrs[4].uuid = &UUID_PAIR_FINISH.u;

    main_chrs[0].uuid = &UUID_AUTH_NONCE.u;
    main_chrs[1].uuid = &UUID_AUTH_PROOF.u;

    config_chrs[0].uuid = &UUID_CONFIG_PARAMS.u;
    config_chrs[1].uuid = &UUID_CONFIG_STATUS.u;
    config_chrs[2].uuid = &UUID_CONFIG_FAN_STATUS.u;
    config_chrs[3].uuid = &UUID_CONFIG_FAN_CALIBRATE.u;

    metrics_chrs[0].uuid = &UUID_TEMP0_VALUE.u;
    metrics_chrs[1].uuid = &UUID_TEMP1_VALUE.u;
    metrics_chrs[2].uuid = &UUID_TEMP2_VALUE.u;
    metrics_chrs[3].uuid = &UUID_TEMP3_VALUE.u;
    metrics_chrs[4].uuid = &UUID_FAN_SPEED_VALUE.u;
}
