#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"

#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

#include "access.h"
#include "conn_guard.h"
#include "crypto.h"
#include "gap.h"
#include "pair_mode.h"
#include "pair_state.h"
#include "state.h"
#include "uuid.h"
#include "load_runtime.h"
#include "load_metrics_ble.h"

/* Only the NimBLE host task reads/writes these lifecycle flags. */
static bool host_synced;
static bool radio_quiet;
static struct ble_npl_event service_event;
static struct ble_npl_event advertise_event;
static struct ble_npl_event stop_advertise_event;
static struct ble_npl_event disconnect_event;
static TaskHandle_t service_waiter;
static void start_advertising_now(void);
static void stop_advertising_cb(struct ble_npl_event *event);

static void disconnect_client_cb(struct ble_npl_event *event) {
    (void)event;
    uint16_t conn = fsm_get_conn_handle();
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void start_advertising_cb(struct ble_npl_event *event) {
    (void)event;
    start_advertising_now();
}

static void service_poll_cb(struct ble_npl_event *event) {
    (void)event;
    if (host_synced) {
        if (load_runtime_window_remaining_ms() > 0) {
            start_advertising_now();
        } else if (radio_quiet && !ble_gap_adv_active() &&
                   !load_runtime_connection_blocks_sleep(
                       fsm_get_conn_handle() != BLE_HS_CONN_HANDLE_NONE)) {
            /* Advertising has completed, so no new connection can be accepted.
             * The sampler is blocked until this callback finishes. */
            load_runtime_sleep();
        }
    }
    xTaskNotifyGive(service_waiter);
}

void gap_init(void) {
    ble_npl_event_init(&service_event, service_poll_cb, NULL);
    ble_npl_event_init(&advertise_event, start_advertising_cb, NULL);
    ble_npl_event_init(&stop_advertise_event, stop_advertising_cb, NULL);
    ble_npl_event_init(&disconnect_event, disconnect_client_cb, NULL);
}

void gap_disconnect_client(void) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &disconnect_event);
}

void gap_service_poll(void) {
    /* Single caller: the load task. One event in flight at a time. */
    if (!service_waiter) {
        service_waiter = xTaskGetCurrentTaskHandle();
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &service_event);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// ====== Terminate timer ======
static void clear_session_secrets_locked(void) {
    memset(auth_nonce, 0, sizeof(auth_nonce));
    memset(host_pub65, 0, sizeof(host_pub65));
    memset(K, 0, sizeof(K));
    memset(host_id_hash, 0, sizeof(host_id_hash));
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

// ====== Advertising / GAP ======
static int gap_event(struct ble_gap_event *event, void *arg);

void start_advertising(void) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &advertise_event);
}

static void start_advertising_now(void) {
    int32_t remaining_ms = load_runtime_window_remaining_ms();
    if (remaining_ms == 0 || !ble_hs_synced() || ble_gap_adv_active() ||
        fsm_get_conn_handle() != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    bool pairing = pair_mode_is_active();

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

    rc = ble_gap_adv_start(g_own_addr_type, NULL, remaining_ms,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return;
    }

    radio_quiet = false;
    ESP_LOGI(TAG, "Advertising started (%s)", pairing ? "PAIR" : "MAIN");
}

void stop_advertising(void) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &stop_advertise_event);
}

static void stop_advertising_cb(struct ble_npl_event *event) {
    (void)event;
    int rc = ble_gap_adv_stop();
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        radio_quiet = true;
    } else {
        ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                radio_quiet = false;
                fsm_dispatch(FSM_EVT_CONNECT, event->connect.conn_handle);
                state_lock();
                clear_session_secrets_locked();
                state_unlock();
                rand_bytes(auth_nonce, sizeof(auth_nonce));
                ESP_LOGI(TAG, "Connected (handle=%d)", event->connect.conn_handle);
            } else {
                radio_quiet = true;
                ESP_LOGI(TAG, "Connect failed; restarting adv");
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT: {
            radio_quiet = true;
            ESP_LOGI(TAG, "Disconnected; restarting adv");
            bool pair_mode_active = pair_mode_is_active();
            fsm_dispatch(FSM_EVT_DISCONNECT, BLE_HS_CONN_HANDLE_NONE);
            state_lock();
            clear_session_secrets_locked();
            state_unlock();
            esp_timer_stop(g_term_timer);
            load_metrics_reset_notify();
            if (pair_mode_active) {
                pair_state_full_reset();
                if (pair_mode_prepare_session()) {
                    start_advertising();
                } else {
                    pair_mode_deactivate();
                    start_advertising();
                }
            } else {
                start_advertising();
            }
            return 0;
        }

        case BLE_GAP_EVENT_ADV_COMPLETE:
            radio_quiet = true;
            /* A new press can extend the window while the old advertising
             * duration is running. Restart only for the remaining time. */
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            load_metrics_set_notify(event->subscribe.attr_handle,
                                    event->subscribe.cur_notify != 0);
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
    host_synced = true;
    radio_quiet = true;
    start_advertising();
}
