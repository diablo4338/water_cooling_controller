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
#include "load_metrics_ble.h"
#include "load_runtime.h"
#include "runtime_config.h"

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
    if (pair_hmac_sha256(K_local, sizeof(K_local), msg, off, expect) != 0) return BLE_ATT_ERR_UNLIKELY;

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

    trust_store_upsert(host_id_hash, K);
    pair_state_set_finish_ok();
    pair_mode_deactivate();

    fsm_set_term_conn_handle(conn_handle);
    if (esp_timer_stop(g_term_timer) == ESP_OK) {
        ESP_LOGI(TAG, "Timer term cancelled reason=restart handle=%u", (unsigned)conn_handle);
    }
    ESP_ERROR_CHECK(esp_timer_start_once(g_term_timer, 250 * 1000));
    ESP_LOGI(TAG, "Timer term started duration_ms=250 handle=%u", (unsigned)conn_handle);

    ESP_LOGI(TAG, "Pairing finished; trusted client added and pairing mode closed");
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

    uint8_t auth_nonce_local[sizeof(auth_nonce)];
    state_lock();
    memcpy(auth_nonce_local, auth_nonce, sizeof(auth_nonce));
    state_unlock();

    if (!trust_store_match_auth(auth_nonce_local, got, NULL)) {
        fsm_dispatch(FSM_EVT_AUTH_FAILED, conn_handle);
        ESP_LOGW(TAG, "AUTH failed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    fsm_dispatch(FSM_EVT_AUTH_OK, conn_handle);
    state_lock();
    memset(auth_nonce, 0, sizeof(auth_nonce));
    state_unlock();
    ESP_LOGI(TAG, "AUTH ok (authed=true)");
    return 0;
}

static uint16_t config_result_attr_handle;
static uint8_t last_config_result[RUNTIME_CONFIG_RESULT_SIZE] = {
    RUNTIME_CONFIG_VERSION, 0, 0, CONFIG_STATUS_OK, CONFIG_FIELD_NONE
};

static int gatt_read_runtime_config(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (!can_access_data() || !auth_conn_check(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE];
    runtime_config_encode(wire, 0);
    return os_mbuf_append(ctxt->om, wire, sizeof(wire)) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_read_config_result(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (!can_access_data() || !auth_conn_check(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    return os_mbuf_append(ctxt->om, last_config_result, sizeof(last_config_result)) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_write_runtime_config(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (!can_access_data() || !auth_conn_check(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    int length = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t wire[RUNTIME_CONFIG_WIRE_SIZE] = {0};
    int copied = length < (int)sizeof(wire) ? length : (int)sizeof(wire);
    if (copied > 0) os_mbuf_copydata(ctxt->om, 0, copied, wire);

    runtime_config_result_t result;
    runtime_config_apply_wire(wire, (size_t)length, &result);
    runtime_config_result_encode(&result, last_config_result);
    struct os_mbuf *notification = ble_hs_mbuf_from_flat(
        last_config_result, sizeof(last_config_result));
    if (notification) {
        int rc = ble_gatts_notify_custom(conn_handle, config_result_attr_handle, notification);
        if (rc != 0) ESP_LOGW(TAG, "Config result notify rc=%d", rc);
    }
    ESP_LOGI(TAG, "Config request=%u status=%u field=%u",
             (unsigned)result.request_id, (unsigned)result.status,
             (unsigned)result.field);
    return 0;
}

static int gatt_access_runtime_config(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return gatt_read_runtime_config(conn_handle, attr_handle, ctxt, arg);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return gatt_write_runtime_config(conn_handle, attr_handle, ctxt, arg);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

typedef enum {
    METRIC_LOAD_STATE,
    METRIC_BUTTON_STATE,
    METRIC_TEMPERATURE,
    METRIC_VOLTAGE_V,
    METRIC_CURRENT_MA,
} metric_id_t;

static int gatt_read_metric(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    if (!can_access_data() || !auth_conn_check(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    load_runtime_snapshot_t snapshot;
    load_runtime_get_snapshot(&snapshot);
    switch ((metric_id_t)(uintptr_t)arg) {
        case METRIC_LOAD_STATE:
            os_mbuf_append(ctxt->om, &snapshot.load_state, sizeof(snapshot.load_state));
            return 0;
        case METRIC_BUTTON_STATE:
            os_mbuf_append(ctxt->om, &snapshot.button_pressed, sizeof(snapshot.button_pressed));
            return 0;
        case METRIC_TEMPERATURE:
            os_mbuf_append(ctxt->om, &snapshot.temperature_c, sizeof(snapshot.temperature_c));
            return 0;
        case METRIC_VOLTAGE_V:
            os_mbuf_append(ctxt->om, &snapshot.voltage_v, sizeof(snapshot.voltage_v));
            return 0;
        case METRIC_CURRENT_MA:
            os_mbuf_append(ctxt->om, &snapshot.current_ma, sizeof(snapshot.current_ma));
            return 0;
        default:
            return BLE_ATT_ERR_UNLIKELY;
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
    { .uuid = NULL, .access_cb = gatt_access_runtime_config,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
    { .uuid = NULL, .access_cb = gatt_read_config_result,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &config_result_attr_handle },
    { 0 }
};

static struct ble_gatt_chr_def metrics_chrs[] = {
    { .uuid = NULL, .access_cb = gatt_read_metric,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &g_load_state_attr_handle, .arg = (void *)(uintptr_t)METRIC_LOAD_STATE },
    { .uuid = NULL, .access_cb = gatt_read_metric,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &g_button_state_attr_handle, .arg = (void *)(uintptr_t)METRIC_BUTTON_STATE },
    { .uuid = NULL, .access_cb = gatt_read_metric,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &g_temperature_attr_handle, .arg = (void *)(uintptr_t)METRIC_TEMPERATURE },
    { .uuid = NULL, .access_cb = gatt_read_metric,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &g_voltage_attr_handle, .arg = (void *)(uintptr_t)METRIC_VOLTAGE_V },
    { .uuid = NULL, .access_cb = gatt_read_metric,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &g_current_attr_handle, .arg = (void *)(uintptr_t)METRIC_CURRENT_MA },
    { 0 }
};

struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = pair_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = main_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = metrics_chrs },
    { 0 }
};

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
    parse_uuid_or_abort(UUID_PAIR_DEV_NONCE_STR, &UUID_PAIR_DEV_NONCE);
    parse_uuid_or_abort(UUID_PAIR_DEV_PUB_STR, &UUID_PAIR_DEV_PUB);
    parse_uuid_or_abort(UUID_PAIR_HOST_PUB_STR, &UUID_PAIR_HOST_PUB);
    parse_uuid_or_abort(UUID_PAIR_CONFIRM_STR, &UUID_PAIR_CONFIRM);
    parse_uuid_or_abort(UUID_PAIR_FINISH_STR, &UUID_PAIR_FINISH);
    parse_uuid_or_abort(UUID_AUTH_NONCE_STR, &UUID_AUTH_NONCE);
    parse_uuid_or_abort(UUID_AUTH_PROOF_STR, &UUID_AUTH_PROOF);
    parse_uuid_or_abort(UUID_RUNTIME_CONFIG_STR, &UUID_RUNTIME_CONFIG);
    parse_uuid_or_abort(UUID_CONFIG_RESULT_STR, &UUID_CONFIG_RESULT);
    parse_uuid_or_abort(UUID_LOAD_STATE_STR, &UUID_LOAD_STATE);
    parse_uuid_or_abort(UUID_BUTTON_STATE_STR, &UUID_BUTTON_STATE);
    parse_uuid_or_abort(UUID_TEMPERATURE_STR, &UUID_TEMPERATURE);
    parse_uuid_or_abort(UUID_VOLTAGE_V_STR, &UUID_VOLTAGE_V);
    parse_uuid_or_abort(UUID_CURRENT_MA_STR, &UUID_CURRENT_MA);

    gatt_svcs[0].uuid = &UUID_PAIR_SVC.u;
    gatt_svcs[1].uuid = &UUID_MAIN_SVC.u;
    gatt_svcs[2].uuid = &UUID_METRICS_SVC.u;
    pair_chrs[0].uuid = &UUID_PAIR_DEV_NONCE.u;
    pair_chrs[1].uuid = &UUID_PAIR_DEV_PUB.u;
    pair_chrs[2].uuid = &UUID_PAIR_HOST_PUB.u;
    pair_chrs[3].uuid = &UUID_PAIR_CONFIRM.u;
    pair_chrs[4].uuid = &UUID_PAIR_FINISH.u;
    main_chrs[0].uuid = &UUID_AUTH_NONCE.u;
    main_chrs[1].uuid = &UUID_AUTH_PROOF.u;
    main_chrs[2].uuid = &UUID_RUNTIME_CONFIG.u;
    main_chrs[3].uuid = &UUID_CONFIG_RESULT.u;
    metrics_chrs[0].uuid = &UUID_LOAD_STATE.u;
    metrics_chrs[1].uuid = &UUID_BUTTON_STATE.u;
    metrics_chrs[2].uuid = &UUID_TEMPERATURE.u;
    metrics_chrs[3].uuid = &UUID_VOLTAGE_V.u;
    metrics_chrs[4].uuid = &UUID_CURRENT_MA.u;
}
