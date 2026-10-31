#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "access.h"
#include "crypto.h"
#include "event_bus.h"
#include "gap.h"
#include "gatt.h"
#include "load_metrics_ble.h"
#include "pair_mode.h"
#include "pair_state.h"
#include "runtime_events.h"
#include "state.h"
#include "uuid.h"

static const char *GAP_TAG = "ble-lifecycle";

static bool s_initialized;
static bool s_controller_enabled;
static bool s_host_stopped = true;
static bool s_radio_quiet = true;
/* A failed one-time service registration cannot be recovered without a full
 * NimBLE teardown, which is deliberately avoided in repeatable awake
 * sessions.  Refuse further starts instead of re-entering a corrupt init. */
static bool s_fatal_error;
static volatile uint32_t s_operation_id;
static TaskHandle_t s_host_task_handle;

static struct ble_npl_event s_advertise_event;
static struct ble_npl_event s_stop_advertise_event;
static struct ble_npl_event s_disconnect_event;

static volatile uint32_t s_adv_duration_ms;
static volatile uint32_t s_adv_operation_id;
static volatile uint32_t s_adv_request_id;
/* Identifies the advertisement session that is actually active in the
 * controller, as opposed to the most recently requested session.  GAP
 * callbacks use these values so a late callback cannot be attributed to a
 * newer advertising generation. */
static volatile uint32_t s_active_adv_operation_id;
static volatile uint32_t s_active_adv_request_id;

static int gap_event(struct ble_gap_event *event, void *arg);
static bool start_advertising_now(uint32_t duration_ms);
static void stop_advertising_now(void);

static void publish_event(runtime_event_kind_t kind, uint32_t operation_id,
                          uint32_t request_id, uint16_t conn_handle,
                          uint16_t reason) {
    runtime_event_t event = runtime_event_make(kind, 0);
    event.operation_id = operation_id;
    event.request_id = request_id;
    event.conn_handle = conn_handle;
    event.reason = reason;
    event_bus_publish(&event);
}

static void advertise_cb(struct ble_npl_event *event) {
    (void)event;
    (void)start_advertising_now(s_adv_duration_ms);
}

static void stop_advertise_cb(struct ble_npl_event *event) {
    (void)event;
    stop_advertising_now();
}

static void disconnect_client_cb(struct ble_npl_event *event) {
    (void)event;
    uint16_t conn = fsm_get_conn_handle();
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(GAP_TAG, "Disconnect requested handle=%u reason=client_reset",
                 (unsigned)conn);
        int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(GAP_TAG, "ble_gap_terminate rc=%d", rc);
    }
}

void gap_init(void) {
    ble_npl_event_init(&s_advertise_event, advertise_cb, NULL);
    ble_npl_event_init(&s_stop_advertise_event, stop_advertise_cb, NULL);
    ble_npl_event_init(&s_disconnect_event, disconnect_client_cb, NULL);
    s_host_stopped = true;
    s_radio_quiet = true;
    s_fatal_error = false;
    s_adv_operation_id = 0;
    s_adv_request_id = 0;
    s_adv_duration_ms = 0;
    s_active_adv_operation_id = 0;
    s_active_adv_request_id = 0;
}

bool gap_is_initialized(void) {
    return s_initialized;
}

bool gap_is_radio_quiet(void) {
    return s_radio_quiet;
}

static void clear_session_secrets_locked(void) {
    memset(auth_nonce, 0, sizeof(auth_nonce));
    memset(host_pub65, 0, sizeof(host_pub65));
    memset(K, 0, sizeof(K));
    memset(host_id_hash, 0, sizeof(host_id_hash));
}

// ====== Terminate timer ======
void term_cb(void *arg) {
    (void)arg;
    uint16_t curr = fsm_get_conn_handle();
    uint16_t expected = fsm_get_term_conn_handle();
    ESP_LOGI(GAP_TAG, "Timer term expired current_handle=%u expected_handle=%u",
             (unsigned)curr, (unsigned)expected);
    if (curr != BLE_HS_CONN_HANDLE_NONE && curr == expected) {
        ESP_LOGI(GAP_TAG, "Terminating conn after finish (handle=%u)",
                 (unsigned)curr);
        int rc = ble_gap_terminate(curr, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(GAP_TAG, "ble_gap_terminate rc=%d", rc);
    } else {
        ESP_LOGW(GAP_TAG, "Skip terminate: stale timer (current=%u expected=%u)",
                 (unsigned)curr, (unsigned)expected);
    }
}

// ====== Advertising / GAP ======
static bool start_advertising_now(uint32_t duration_ms) {
    if (duration_ms == 0 || !ble_hs_synced() ||
        fsm_get_conn_handle() != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(GAP_TAG, "Advertising start rejected duration=%u synced=%d conn=%u",
                 (unsigned)duration_ms, (int)ble_hs_synced(),
                 (unsigned)fsm_get_conn_handle());
        publish_event(RTEVT_BLE_START_FAILED, s_adv_operation_id,
                      s_adv_request_id, BLE_HS_CONN_HANDLE_NONE, 0);
        return false;
    }

    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(GAP_TAG, "ble_gap_adv_stop before restart rc=%d", rc);
        }
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
    int rc = ble_hs_adv_set_fields(&adv_fields, adv_data, &adv_len,
                                   sizeof(adv_data));
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_hs_adv_set_fields rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, s_adv_operation_id,
                      s_adv_request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }
    rc = ble_gap_adv_set_data(adv_data, adv_len);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gap_adv_set_data rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, s_adv_operation_id,
                      s_adv_request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    const char *name = pairing ? "sensor-pair" : "sensor";
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = (uint8_t)strlen(name);
    rsp_fields.name_is_complete = 1;

    uint8_t rsp_data[31];
    uint8_t rsp_len = 0;
    rc = ble_hs_adv_set_fields(&rsp_fields, rsp_data, &rsp_len,
                               sizeof(rsp_data));
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_hs_adv_set_fields (rsp) rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, s_adv_operation_id,
                      s_adv_request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }
    rc = ble_gap_adv_rsp_set_data(rsp_data, rsp_len);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gap_adv_rsp_set_data rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, s_adv_operation_id,
                      s_adv_request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }

    rc = ble_gap_adv_start(g_own_addr_type, NULL, (int32_t)duration_ms,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gap_adv_start rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, s_adv_operation_id,
                      s_adv_request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }
    s_active_adv_operation_id = s_adv_operation_id;
    s_active_adv_request_id = s_adv_request_id;
    s_radio_quiet = false;
    ESP_LOGI(GAP_TAG, "Advertising started (%s) duration_ms=%u req=%u",
             pairing ? "PAIR" : "MAIN", (unsigned)duration_ms,
             (unsigned)s_adv_request_id);
    return true;
}

static void stop_advertising_now(void) {
    int rc = ble_gap_adv_stop();
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_radio_quiet = true;
    } else {
        ESP_LOGW(GAP_TAG, "ble_gap_adv_stop rc=%d", rc);
    }
    s_active_adv_operation_id = 0;
    s_active_adv_request_id = 0;
}

bool gap_start_advertising(uint32_t operation_id, uint32_t request_id,
                           uint32_t duration_ms) {
    if (!s_initialized || s_host_stopped ||
        nimble_port_get_dflt_eventq() == NULL) {
        return false;
    }
    s_adv_operation_id = operation_id;
    s_adv_request_id = request_id;
    s_adv_duration_ms = duration_ms;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_advertise_event);
    return true;
}

bool gap_refresh_advertising(uint32_t operation_id, uint32_t request_id,
                             uint32_t duration_ms) {
    return gap_start_advertising(operation_id, request_id, duration_ms);
}

void gap_stop_advertising(void) {
    if (!s_initialized || s_host_stopped ||
        nimble_port_get_dflt_eventq() == NULL) {
        s_radio_quiet = true;
        return;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_stop_advertise_event);
}

void gap_disconnect_client(void) {
    if (!s_initialized || s_host_stopped ||
        nimble_port_get_dflt_eventq() == NULL) {
        return;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_disconnect_event);
}

// ====== Host lifecycle ======
static bool setup_services(void) {
    gatt_init_uuids_and_services();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("sensor");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gatts_count_cfg rc=%d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gatts_add_svcs rc=%d", rc);
        return false;
    }
    return true;
}

bool gap_ble_start(uint32_t operation_id) {
    s_operation_id = operation_id;

    if (s_fatal_error) return false;

    if (!s_initialized) {
        esp_err_t err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(GAP_TAG, "nimble_port_init failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        s_initialized = true;
        s_controller_enabled = true;
        if (!setup_services()) {
            (void)esp_bt_controller_disable();
            s_controller_enabled = false;
            s_initialized = false;
            s_fatal_error = true;
            return false;
        }
        ble_hs_cfg.sync_cb = ble_app_on_sync;
        s_host_stopped = false;
        s_radio_quiet = false;
        nimble_port_freertos_init(ble_host_task);
        ESP_LOGI(GAP_TAG, "NimBLE initialized lazily operation=%u",
                 (unsigned)operation_id);
        return true;
    }

    if (s_host_stopped) {
        if (!s_controller_enabled) {
            esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
            if (err != ESP_OK) {
                ESP_LOGE(GAP_TAG, "controller enable failed: %s",
                         esp_err_to_name(err));
                return false;
            }
            s_controller_enabled = true;
        }
        if (!s_host_task_handle) return false;
        s_host_stopped = false;
        s_radio_quiet = false;
        xTaskNotifyGive(s_host_task_handle);
        ESP_LOGI(GAP_TAG, "NimBLE host restart operation=%u",
                 (unsigned)operation_id);
        return true;
    }

    /* Already running; this can happen for a duplicate lifecycle command.
     * Re-publish READY only when the host is already synced so the BLE FSM
     * cannot remain stuck in STARTING. */
    if (ble_hs_synced()) {
        publish_event(RTEVT_BLE_READY, s_operation_id, 0,
                      BLE_HS_CONN_HANDLE_NONE, 0);
    }
    return true;
}

static bool wait_host_stopped(uint32_t timeout_ms) {
    for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (s_host_stopped) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return s_host_stopped;
}

bool gap_ble_stop(uint32_t operation_id) {
    (void)operation_id;
    if (!s_initialized) {
        s_radio_quiet = true;
        s_active_adv_operation_id = 0;
        s_active_adv_request_id = 0;
        return true;
    }

    if (!s_host_stopped) {
        int rc = nimble_port_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(GAP_TAG, "nimble_port_stop rc=%d", rc);
            return false;
        }
        if (!wait_host_stopped(2000)) {
            ESP_LOGE(GAP_TAG, "host task did not stop in time");
            return false;
        }
    }
    if (s_controller_enabled) {
        esp_err_t err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGE(GAP_TAG, "controller disable failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        s_controller_enabled = false;
    }
    s_radio_quiet = true;
    s_active_adv_operation_id = 0;
    s_active_adv_request_id = 0;
    ESP_LOGI(GAP_TAG, "NimBLE stopped, radio quiet");
    return true;
}

void ble_host_task(void *param) {
    (void)param;
    s_host_task_handle = xTaskGetCurrentTaskHandle();
    for (;;) {
        nimble_port_run();
        s_host_stopped = true;
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            s_host_stopped = false;
            int rc = ble_hs_start();
            if (rc == 0 || rc == BLE_HS_EALREADY) {
                break;
            }
            ESP_LOGE(GAP_TAG, "ble_hs_start rc=%d", rc);
            s_host_stopped = true;
            publish_event(RTEVT_BLE_START_FAILED, s_operation_id, 0,
                          BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        }
    }
}

// ====== GAP callbacks ======
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_radio_quiet = false;
                publish_event(RTEVT_CLIENT_CONNECTED,
                              s_active_adv_operation_id,
                              s_active_adv_request_id,
                              event->connect.conn_handle, 0);
                s_active_adv_operation_id = 0;
                s_active_adv_request_id = 0;
                /* Pairing/auth FSM updates must happen before GATT callbacks. */
                fsm_dispatch(FSM_EVT_CONNECT, event->connect.conn_handle);
                state_lock();
                clear_session_secrets_locked();
                state_unlock();
                rand_bytes(auth_nonce, sizeof(auth_nonce));
                ESP_LOGI(GAP_TAG, "Connected (handle=%d)",
                         event->connect.conn_handle);
            } else {
                s_radio_quiet = true;
                ESP_LOGW(GAP_TAG, "Connect failed status=%d",
                         event->connect.status);
                publish_event(RTEVT_ADV_COMPLETE,
                              s_active_adv_operation_id,
                              s_active_adv_request_id,
                              BLE_HS_CONN_HANDLE_NONE,
                              (uint16_t)event->connect.status);
                s_active_adv_operation_id = 0;
                s_active_adv_request_id = 0;
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            s_radio_quiet = true;
            ESP_LOGI(GAP_TAG, "Disconnected handle=%u reason=0x%X",
                     (unsigned)event->disconnect.conn.conn_handle,
                     (unsigned)event->disconnect.reason);
            publish_event(RTEVT_CLIENT_DISCONNECTED, s_operation_id,
                          s_adv_request_id,
                          event->disconnect.conn.conn_handle,
                          (uint16_t)event->disconnect.reason);
            pair_mode_deactivate();
            fsm_dispatch(FSM_EVT_DISCONNECT, BLE_HS_CONN_HANDLE_NONE);
            state_lock();
            clear_session_secrets_locked();
            state_unlock();
            if (esp_timer_stop(g_term_timer) == ESP_OK) {
                ESP_LOGI(GAP_TAG, "Timer term cancelled reason=disconnected");
            }
            load_metrics_reset_notify();
            pair_state_full_reset();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(GAP_TAG, "Advertising completed reason=%d",
                     event->adv_complete.reason);
            s_radio_quiet = true;
            publish_event(RTEVT_ADV_COMPLETE, s_active_adv_operation_id,
                          s_active_adv_request_id, BLE_HS_CONN_HANDLE_NONE,
                          (uint16_t)event->adv_complete.reason);
            s_active_adv_operation_id = 0;
            s_active_adv_request_id = 0;
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
        ESP_LOGE(GAP_TAG, "ble_hs_id_infer_auto rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, s_operation_id, 0,
                      BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return;
    }
    ESP_LOGI(GAP_TAG, "BLE host synced addr_type=%u operation=%u",
             (unsigned)g_own_addr_type, (unsigned)s_operation_id);
    s_radio_quiet = false;
    publish_event(RTEVT_BLE_READY, s_operation_id, 0,
                  BLE_HS_CONN_HANDLE_NONE, 0);
}
