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
#include "device_status.h"
#include "device_status_ble.h"
#include "metrics.h"
#include "metrics_ble.h"
#include "fan_control.h"
#include "fan_status_ble.h"
#include "operation_manager.h"
#include "output_limit.h"
#include "operation_status_ble.h"
#include "params.h"
#include "profiles.h"

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
    if (!pair_state_can_dispatch(PAIR_EVT_HOST_PUB_ACCEPTED)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
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

    if (!pair_state_dispatch(PAIR_EVT_HOST_PUB_ACCEPTED)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    ESP_LOGI(TAG, "Host pub received; K derived");
    return 0;
}

static int gatt_write_pair_confirm(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pair_state_can_dispatch(PAIR_EVT_CONFIRM_ACCEPTED)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
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

    if (!pair_state_dispatch(PAIR_EVT_CONFIRM_ACCEPTED)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    ESP_LOGI(TAG, "PAIR_CONFIRM ok");
    return 0;
}

static int gatt_write_pair_finish(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_pairing()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pair_state_can_dispatch(PAIR_EVT_FINISH_ACCEPTED)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!pairing_conn_check(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t b;
    os_mbuf_copydata(ctxt->om, 0, 1, &b);
    if (b != 0x01) return BLE_ATT_ERR_UNLIKELY;

    if (!trust_store_upsert(host_id_hash, K)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!pair_state_dispatch(PAIR_EVT_FINISH_ACCEPTED)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    pair_mode_dispatch(PAIR_WINDOW_PAIR_COMPLETED);

    fsm_set_term_conn_handle(conn_handle);
    esp_timer_stop(g_term_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(g_term_timer, 250 * 1000));

    ESP_LOGI(TAG, "Pairing finished; trusted client added and pairing mode closed");
    return 0;
}

static int gatt_read_auth_nonce(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (!can_access_auth_nonce(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
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

    if (!can_access_auth_nonce(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    if (can_access_data(conn_handle)) {
        ESP_LOGI(TAG, "AUTH already ok; ignoring repeat proof");
        return 0;
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

    if (!fsm_dispatch(FSM_EVT_AUTH_OK, conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    ESP_LOGI(TAG, "AUTH ok (authed=true)");
    return 0;
}

// ====== Metrics ======
static int gatt_read_temp_single(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t channel = (uint8_t)(uintptr_t)arg;
    float temp = metrics_get_temp(channel);
    os_mbuf_append(ctxt->om, &temp, sizeof(temp));
    return 0;
}

static int gatt_read_fan_speed(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t channel = (uint8_t)(uintptr_t)arg;
    float rpm = metrics_get_fan_speed_rpm_channel(channel);
    os_mbuf_append(ctxt->om, &rpm, sizeof(rpm));
    return 0;
}

static int gatt_read_voltage(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    float voltage = metrics_get_voltage_v();
    os_mbuf_append(ctxt->om, &voltage, sizeof(voltage));
    return 0;
}

static int gatt_read_current(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    float current = metrics_get_current_ma();
    os_mbuf_append(ctxt->om, &current, sizeof(current));
    return 0;
}

static int gatt_read_fan_percent(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    float percent = fan_control_get_output_percent();
    os_mbuf_append(ctxt->om, &percent, sizeof(percent));
    return 0;
}

// ====== Config params ======
static int gatt_read_params(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t payload[PARAMS_PAYLOAD_LEN];
    if (!params_get_current_payload(payload, sizeof(payload))) return BLE_ATT_ERR_UNLIKELY;
    os_mbuf_append(ctxt->om, payload, sizeof(payload));
    return 0;
}

static int gatt_write_params(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (operation_manager_is_busy()) {
        params_set_last_status(PARAM_STATUS_BUSY, PARAM_FIELD_NONE);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != PARAMS_PAYLOAD_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[PARAMS_PAYLOAD_LEN];
    os_mbuf_copydata(ctxt->om, 0, sizeof(buf), buf);
    if (!params_set_pending_payload(conn_handle, buf, sizeof(buf))) return BLE_ATT_ERR_UNLIKELY;
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

static void gatt_notify_params_status(uint16_t conn_handle) {
    if (g_params_status_attr_handle == 0) {
        return;
    }
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

static int gatt_access_params_status(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

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

        if (operation_manager_is_busy()) {
            params_set_last_status(PARAM_STATUS_BUSY, PARAM_FIELD_NONE);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        params_apply(conn_handle);

        gatt_notify_params_status(conn_handle);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access_profiles(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t mask = 0;
        if (!profiles_read_mask(&mask)) return BLE_ATT_ERR_UNLIKELY;
        const uint8_t payload[PROFILE_STATUS_PAYLOAD_LEN] = {
            PROFILE_STATUS_VERSION,
            mask,
        };
        os_mbuf_append(ctxt->om, payload, sizeof(payload));
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (OS_MBUF_PKTLEN(ctxt->om) != 2) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t command[2];
    os_mbuf_copydata(ctxt->om, 0, sizeof(command), command);
    uint8_t action = command[0];
    uint8_t slot = command[1];
    if (slot < 1 || slot > PROFILE_COUNT) return BLE_ATT_ERR_UNLIKELY;
    if (operation_manager_is_busy()) {
        params_set_last_status(PARAM_STATUS_BUSY, PARAM_FIELD_NONE);
        gatt_notify_params_status(conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (action == PROFILE_ACTION_SAVE_CURRENT) {
        return profiles_save_current(slot) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    if (action == PROFILE_ACTION_APPLY) {
        if (!profiles_apply(slot)) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        gatt_notify_params_status(conn_handle);
        return 0;
    }
    if (action == PROFILE_ACTION_CLEAR) {
        return profiles_clear(slot) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access_profile_metadata(uint16_t conn_handle, uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    uint8_t slot = (uint8_t)(uintptr_t)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t payload[PROFILE_METADATA_MAX_LEN];
        size_t len = 0;
        if (!profiles_read_metadata(slot, payload, sizeof(payload), &len)) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, payload, (uint16_t)len) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 3 || len > PROFILE_METADATA_MAX_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t payload[PROFILE_METADATA_MAX_LEN];
    if (os_mbuf_copydata(ctxt->om, 0, len, payload) != 0 ||
        !profiles_write_metadata(slot, payload, (size_t)len)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static int gatt_read_fan_status(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t payload[FAN_STATUS_PAYLOAD_LEN];
    fan_control_get_status_payload(payload, sizeof(payload));
    os_mbuf_append(ctxt->om, payload, sizeof(payload));
    return 0;
}

static int gatt_read_device_status(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t payload[DEVICE_STATUS_PAYLOAD_LEN];
    device_status_get_payload(payload, sizeof(payload));
    os_mbuf_append(ctxt->om, payload, sizeof(payload));
    return 0;
}

// ====== Operations ======
static int gatt_access_operation_control(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 3) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[3];
    os_mbuf_copydata(ctxt->om, 0, sizeof(buf), buf);
    uint8_t version = buf[0];
    uint8_t op_type = buf[1];
    uint8_t action = buf[2];
    if (version != OP_CONTROL_VERSION) return BLE_ATT_ERR_UNLIKELY;
    if (action == OP_CONTROL_CANCEL && op_type == OP_TYPE_NONE) {
        return operation_manager_cancel(conn_handle) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (action != OP_CONTROL_START) return BLE_ATT_ERR_UNLIKELY;

    switch (operation_manager_start((operation_type_t)op_type, conn_handle)) {
        case OP_START_OK:
            return 0;
        case OP_START_BUSY:
        case OP_START_OVERHEAT:
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gatt_access_operation_status(uint16_t conn_handle, uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t payload[OP_STATUS_PAYLOAD_LEN];
    operation_manager_get_status_payload(payload, sizeof(payload));
    os_mbuf_append(ctxt->om, payload, sizeof(payload));
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
    { 0 }
};

static int gatt_access_output_limit(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    bool extended = arg != NULL;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t payload[OUTPUT_LIMIT_EXTENDED_LEN];
        size_t len = output_limit_status_encode(extended, payload);
        return os_mbuf_append(ctxt->om, payload, len) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (extended || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t command[3];
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(command)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, sizeof(command), command) != 0 ||
        command[0] != OUTPUT_LIMIT_VERSION) return BLE_ATT_ERR_UNLIKELY;
    if (operation_manager_is_busy()) {
        if (command[1] == OUTPUT_LIMIT_WRITE_MAX) params_set_last_status(PARAM_STATUS_BUSY, PARAM_FIELD_NONE);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (command[1] == OUTPUT_LIMIT_WRITE_MAX) {
        uint8_t result = params_set_output_max(command[2]);
        if (result == PARAM_STATUS_OUTPUT_ADJUSTING) return BLE_ATT_ERR_INSUFFICIENT_RES;
        return result == PARAM_STATUS_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    if (command[1] == OUTPUT_LIMIT_RESET && command[2] == 0) {
        return output_limit_request_reset() ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access_fan_mode_status(uint16_t conn_handle, uint16_t attr_handle,
                                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t payload[FAN_MODE_STATUS_PAYLOAD_LEN];
    fan_control_get_mode_payload(payload, sizeof(payload));
    return os_mbuf_append(ctxt->om, payload, sizeof(payload)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access_profile_result(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;
    if (!can_access_data(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t payload[PROFILE_RESULT_PAYLOAD_LEN];
    profiles_get_result_payload(payload);
    return os_mbuf_append(ctxt->om, payload, sizeof(payload)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

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
        .access_cb = gatt_read_device_status,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_device_status_attr_handle,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_profiles,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_profile_metadata,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .arg = (void *)(uintptr_t)1,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_profile_metadata,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .arg = (void *)(uintptr_t)2,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_profile_metadata,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .arg = (void *)(uintptr_t)3,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_output_limit,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_output_limit_attr_handle,
    },
    {
        .access_cb = gatt_access_fan_mode_status,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_mode_status_attr_handle,
    },
    {
        .access_cb = gatt_access_profile_result,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .access_cb = gatt_access_output_limit,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .arg = (void *)(uintptr_t)1,
        .val_handle = &g_output_range_attr_handle,
    },
    { 0 }
};

static struct ble_gatt_chr_def operations_chrs[] = {
    {
        .uuid = NULL,
        .access_cb = gatt_access_operation_control,
        .flags = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_access_operation_status,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_operation_status_attr_handle,
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
        .val_handle = &g_fan_attr_handles[0],
        .arg = (void *)(uintptr_t)0,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_fan_speed,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_attr_handles[1],
        .arg = (void *)(uintptr_t)1,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_fan_speed,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_attr_handles[2],
        .arg = (void *)(uintptr_t)2,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_fan_speed,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_attr_handles[3],
        .arg = (void *)(uintptr_t)3,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_voltage,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_voltage_attr_handle,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_current,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_current_attr_handle,
    },
    {
        .uuid = NULL,
        .access_cb = gatt_read_fan_percent,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_fan_percent_attr_handle,
    },
    { 0 }
};

struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = pair_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = main_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = config_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NULL, .characteristics = operations_chrs },
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
    parse_uuid_or_abort(UUID_OPERATIONS_SVC_STR, &UUID_OPERATIONS_SVC);

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
    parse_uuid_or_abort(UUID_CONFIG_DEVICE_STATUS_STR, &UUID_CONFIG_DEVICE_STATUS);
    parse_uuid_or_abort(UUID_CONFIG_PROFILES_STR, &UUID_CONFIG_PROFILES);
    parse_uuid_or_abort(UUID_CONFIG_PROFILE1_METADATA_STR, &UUID_CONFIG_PROFILE1_METADATA);
    parse_uuid_or_abort(UUID_CONFIG_PROFILE2_METADATA_STR, &UUID_CONFIG_PROFILE2_METADATA);
    parse_uuid_or_abort(UUID_CONFIG_PROFILE3_METADATA_STR, &UUID_CONFIG_PROFILE3_METADATA);
    parse_uuid_or_abort(UUID_CONFIG_OUTPUT_LIMIT_STR, &UUID_CONFIG_OUTPUT_LIMIT);
    parse_uuid_or_abort(UUID_CONFIG_OUTPUT_RANGE_STR, &UUID_CONFIG_OUTPUT_RANGE);
    parse_uuid_or_abort(UUID_CONFIG_FAN_MODE_STATUS_STR, &UUID_CONFIG_FAN_MODE_STATUS);
    parse_uuid_or_abort(UUID_CONFIG_PROFILE_RESULT_STR, &UUID_CONFIG_PROFILE_RESULT);
    parse_uuid_or_abort(UUID_OP_CONTROL_STR, &UUID_OP_CONTROL);
    parse_uuid_or_abort(UUID_OP_STATUS_STR, &UUID_OP_STATUS);
    
    parse_uuid_or_abort(UUID_TEMP0_VALUE_STR, &UUID_TEMP0_VALUE);
    parse_uuid_or_abort(UUID_TEMP1_VALUE_STR, &UUID_TEMP1_VALUE);
    parse_uuid_or_abort(UUID_TEMP2_VALUE_STR, &UUID_TEMP2_VALUE);
    parse_uuid_or_abort(UUID_TEMP3_VALUE_STR, &UUID_TEMP3_VALUE);
    parse_uuid_or_abort(UUID_FAN_SPEED_VALUE_STR, &UUID_FAN_SPEED_VALUE);
    parse_uuid_or_abort(UUID_FAN2_SPEED_VALUE_STR, &UUID_FAN2_SPEED_VALUE);
    parse_uuid_or_abort(UUID_FAN3_SPEED_VALUE_STR, &UUID_FAN3_SPEED_VALUE);
    parse_uuid_or_abort(UUID_FAN4_SPEED_VALUE_STR, &UUID_FAN4_SPEED_VALUE);
    parse_uuid_or_abort(UUID_VOLTAGE_VALUE_STR, &UUID_VOLTAGE_VALUE);
    parse_uuid_or_abort(UUID_CURRENT_VALUE_STR, &UUID_CURRENT_VALUE);
    parse_uuid_or_abort(UUID_FAN_PERCENT_VALUE_STR, &UUID_FAN_PERCENT_VALUE);

    gatt_svcs[0].uuid = &UUID_PAIR_SVC.u;
    gatt_svcs[1].uuid = &UUID_MAIN_SVC.u;
    gatt_svcs[2].uuid = &UUID_CONFIG_SVC.u;
    gatt_svcs[3].uuid = &UUID_OPERATIONS_SVC.u;
    gatt_svcs[4].uuid = &UUID_METRICS_SVC.u;

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
    config_chrs[3].uuid = &UUID_CONFIG_DEVICE_STATUS.u;
    config_chrs[4].uuid = &UUID_CONFIG_PROFILES.u;
    config_chrs[5].uuid = &UUID_CONFIG_PROFILE1_METADATA.u;
    config_chrs[6].uuid = &UUID_CONFIG_PROFILE2_METADATA.u;
    config_chrs[7].uuid = &UUID_CONFIG_PROFILE3_METADATA.u;
    config_chrs[8].uuid = &UUID_CONFIG_OUTPUT_LIMIT.u;
    config_chrs[9].uuid = &UUID_CONFIG_FAN_MODE_STATUS.u;
    config_chrs[10].uuid = &UUID_CONFIG_PROFILE_RESULT.u;
    config_chrs[11].uuid = &UUID_CONFIG_OUTPUT_RANGE.u;
    operations_chrs[0].uuid = &UUID_OP_CONTROL.u;
    operations_chrs[1].uuid = &UUID_OP_STATUS.u;

    metrics_chrs[0].uuid = &UUID_TEMP0_VALUE.u;
    metrics_chrs[1].uuid = &UUID_TEMP1_VALUE.u;
    metrics_chrs[2].uuid = &UUID_TEMP2_VALUE.u;
    metrics_chrs[3].uuid = &UUID_TEMP3_VALUE.u;
    metrics_chrs[4].uuid = &UUID_FAN_SPEED_VALUE.u;
    metrics_chrs[5].uuid = &UUID_FAN2_SPEED_VALUE.u;
    metrics_chrs[6].uuid = &UUID_FAN3_SPEED_VALUE.u;
    metrics_chrs[7].uuid = &UUID_FAN4_SPEED_VALUE.u;
    metrics_chrs[8].uuid = &UUID_VOLTAGE_VALUE.u;
    metrics_chrs[9].uuid = &UUID_CURRENT_VALUE.u;
    metrics_chrs[10].uuid = &UUID_FAN_PERCENT_VALUE.u;
}
