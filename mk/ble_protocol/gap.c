#include <string.h>

#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

#include "access.h"
#include "conn_guard.h"
#include "crypto.h"
#include "gap.h"
#include "pair_state.h"
#include "state.h"
#include "uuid.h"
#include "rgb.h"

// ====== Terminate timer ======
static void clear_session_secrets_locked(void) {
    memset(dev_nonce, 0, sizeof(dev_nonce));
    memset(auth_nonce, 0, sizeof(auth_nonce));
    memset(host_pub65, 0, sizeof(host_pub65));
}

void term_cb(void *arg) {
    (void)arg;
    uint16_t curr = fsm_get_conn_handle();
    uint16_t expected = fsm_get_term_conn_handle();
    if (curr != BLE_HS_CONN_HANDLE_NONE && curr == expected) {
        ESP_LOGI(TAG, "Terminating conn after finish (handle=%u)", (unsigned)curr);
        int rc = ble_gap_terminate(curr, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
    } else {
        ESP_LOGW(TAG, "Skip terminate: stale timer (current=%u expected=%u)",
                 (unsigned)curr, (unsigned)expected);
    }
}

void data_timer_cb(void *arg) {
    (void)arg;

    uint16_t conn;
    bool notify;
    uint16_t attr = g_data_attr_handle;
    conn = fsm_get_conn_handle();
    notify = fsm_get_data_notify_enabled();

    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (!auth_conn_check(conn)) return;
    if (!notify) return;
    if (attr == 0) return;
    if (!can_access_data()) return;

    uint8_t payload[20];
    rand_bytes(payload, sizeof(payload));

    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (!om) return;

    rgb_notify_pulse();
    int rc = ble_gatts_notify_custom(conn, attr, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify_custom rc=%d", rc);
    }
}

// ====== Advertising / GAP ======
static int gap_event(struct ble_gap_event *event, void *arg);

void start_advertising(void) {
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    bool pairing = fsm_is_pairing();

    if (pairing) {
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

    const char *name = pairing ? "sensor-pair" : "sensor";
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

    ESP_LOGI(TAG, "Advertising started (%s)", pairing ? "PAIR" : "MAIN");
}

void stop_advertising(void) {
    int rc = ble_gap_adv_stop();
    if (rc != 0) ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                fsm_dispatch(FSM_EVT_CONNECT, event->connect.conn_handle);
                state_lock();
                clear_session_secrets_locked();
                state_unlock();
                rand_bytes(auth_nonce, sizeof(auth_nonce));
                rgb_set_connected(true);
                ESP_LOGI(TAG, "Connected (handle=%d)", event->connect.conn_handle);

                esp_timer_stop(g_data_timer);
                ESP_ERROR_CHECK(esp_timer_start_periodic(g_data_timer, 1000 * 1000));
            } else {
                ESP_LOGI(TAG, "Connect failed; restarting adv");
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGI(TAG, "Disconnected; restarting adv");
            bool was_pairing = fsm_is_pairing();
            fsm_dispatch(FSM_EVT_DISCONNECT, BLE_HS_CONN_HANDLE_NONE);
            state_lock();
            clear_session_secrets_locked();
            state_unlock();
            esp_timer_stop(g_data_timer);
            esp_timer_stop(g_term_timer);
            if (was_pairing) {
                pair_state_full_reset();
                esp_timer_stop(g_pair_timer);
            }
            rgb_set_connected(false);
            rgb_set_pairing(was_pairing);
            start_advertising();
            return 0;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_data_attr_handle) {
                bool enabled = (event->subscribe.cur_notify != 0);
                fsm_set_data_notify_enabled(enabled);
                ESP_LOGI(TAG, "DATA notify: %s", enabled ? "ON" : "OFF");
            }
            return 0;

        default:
            return 0;
    }
}

// ====== Sync ======
void ble_app_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }
    start_advertising();
}
