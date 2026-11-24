#include <string.h>

#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_stop.h"
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
static bool s_controller_initialized;
static bool s_controller_enabled;
static SemaphoreHandle_t s_host_stopped;
static StaticSemaphore_t s_metrics_mutex_storage;
static SemaphoreHandle_t s_metrics_mutex;
static bool s_metrics_enabled;
static TaskHandle_t s_host_task;
static bool s_stop_pending;
static bool s_stop_reported;
static int s_stop_status;
static struct ble_hs_stop_listener s_stop_listener;
static struct ble_npl_event s_stop_event;
static struct ble_npl_event s_quiesce_event;
/* Partial deinitialization is unsafe to reuse. */
static bool s_fatal_error;
static volatile uint32_t s_operation_id;
static struct ble_npl_event s_advertise_event;
static struct ble_npl_event s_disconnect_event;
static portMUX_TYPE s_disconnect_mux = portMUX_INITIALIZER_UNLOCKED;
static runtime_event_t s_undelivered_disconnect;
/* The authorization FSM can be reset while the physical link is still up. */
static uint16_t s_link_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_disconnect_handle;

typedef struct {
    uint32_t duration_ms;
    uint32_t operation_id;
    uint32_t request_id;
} advertising_request_t;

static portMUX_TYPE s_adv_mux = portMUX_INITIALIZER_UNLOCKED;
static advertising_request_t s_adv_request;
/* Identifies the advertisement session that is actually active in the
 * controller, as opposed to the most recently requested session.  GAP
 * callbacks use these values so a late callback cannot be attributed to a
 * newer advertising generation. */
static volatile uint32_t s_active_adv_operation_id;
static volatile uint32_t s_active_adv_request_id;

static int gap_event(struct ble_gap_event *event, void *arg);
static bool start_advertising_now(advertising_request_t request);

static void publish_event(runtime_event_kind_t kind, uint32_t operation_id,
                          uint32_t request_id, uint16_t conn_handle,
                          uint16_t reason) {
    runtime_event_t event = runtime_event_make(kind, 0);
    event.operation_id = operation_id;
    event.request_id = request_id;
    event.conn_handle = conn_handle;
    event.reason = reason;
    event.monotonic_timestamp_us = esp_timer_get_time();
    event_bus_publish(&event);
}

static void host_reset_cb(int reason) {
    ESP_LOGW(GAP_TAG, "NimBLE host reset reason=%d", reason);
    publish_event(RTEVT_BLE_HOST_RESET, s_operation_id, 0,
                  BLE_HS_CONN_HANDLE_NONE, (uint16_t)reason);
}

static void advertise_cb(struct ble_npl_event *event) {
    (void)event;
    portENTER_CRITICAL(&s_adv_mux);
    advertising_request_t request = s_adv_request;
    portEXIT_CRITICAL(&s_adv_mux);
    (void)start_advertising_now(request);
}

static void disconnect_client_cb(struct ble_npl_event *event) {
    (void)event;
    uint16_t conn = s_disconnect_handle;
    if (conn != BLE_HS_CONN_HANDLE_NONE && conn == s_link_handle) {
        ESP_LOGI(GAP_TAG, "Disconnect requested handle=%u reason=client_reset",
                 (unsigned)conn);
        int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) ESP_LOGW(GAP_TAG, "ble_gap_terminate rc=%d", rc);
    }
}

static void quiesce_host_cb(struct ble_npl_event *event) {
    (void)event;
    bool stopped = s_stop_status == 0;
    xSemaphoreGive(s_host_stopped);
    if (stopped) vTaskSuspend(NULL);
}

static void host_stop_done(int status, void *arg) {
    (void)arg;
    s_stop_reported = true;
    s_stop_status = status;
    /* Drain the current host callback before the executor deletes the task. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_quiesce_event);
}

static void stop_host_cb(struct ble_npl_event *event) {
    (void)event;
    s_stop_reported = false;
    int rc = ble_hs_stop(&s_stop_listener, host_stop_done, NULL);
    if (rc != 0 && !s_stop_reported) {
        host_stop_done(rc == BLE_HS_EALREADY ? 0 : rc, NULL);
    }
}

void gap_init(void) {
    s_metrics_mutex = xSemaphoreCreateMutexStatic(&s_metrics_mutex_storage);
    s_fatal_error = false;
    s_adv_request = (advertising_request_t){0};
    s_active_adv_operation_id = 0;
    s_active_adv_request_id = 0;
}

void gap_publish_metrics(void) {
    if (xSemaphoreTake(s_metrics_mutex, 0) != pdTRUE) return;
    if (s_metrics_enabled) load_metrics_publish();
    xSemaphoreGive(s_metrics_mutex);
}

static void log_memory(const char *phase, unsigned host_stack_free) {
    ESP_LOGW(GAP_TAG, "%s heap_free=%u heap_min=%u largest_block=%u host_stack_free=%u",
             phase, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             host_stack_free);
}

bool gap_take_pending_disconnect(runtime_event_t *out) {
    portENTER_CRITICAL(&s_disconnect_mux);
    bool pending = s_undelivered_disconnect.kind == RTEVT_CLIENT_DISCONNECTED;
    if (pending) {
        *out = s_undelivered_disconnect;
        s_undelivered_disconnect.kind = RTEVT_NONE;
    }
    portEXIT_CRITICAL(&s_disconnect_mux);
    return pending;
}

bool gap_is_initialized(void) {
    return s_initialized;
}

bool gap_is_radio_quiet(void) {
    return !s_initialized && !s_controller_enabled && !s_fatal_error;
}

static void clear_session_secrets_locked(void) {
    memset(auth_nonce, 0, sizeof(auth_nonce));
    memset(host_pub65, 0, sizeof(host_pub65));
    memset(K, 0, sizeof(K));
    memset(host_id_hash, 0, sizeof(host_id_hash));
}

static void clear_link(void) {
    s_link_handle = BLE_HS_CONN_HANDLE_NONE;
    fsm_dispatch(FSM_EVT_DISCONNECT, BLE_HS_CONN_HANDLE_NONE);
    state_lock();
    clear_session_secrets_locked();
    state_unlock();
    (void)esp_timer_stop(g_term_timer);
    load_metrics_reset_notify();
    pair_state_full_reset();
}

// ====== Terminate timer ======
void term_cb(void *arg) {
    (void)arg;
    uint16_t curr = fsm_get_conn_handle();
    uint16_t expected = fsm_get_term_conn_handle();
    ESP_LOGI(GAP_TAG, "Timer term expired current_handle=%u expected_handle=%u",
             (unsigned)curr, (unsigned)expected);
    if (curr != BLE_HS_CONN_HANDLE_NONE && curr == expected) {
        publish_event(RTEVT_BLE_PAIR_FINISH_DISCONNECT, s_operation_id, 0, curr, 0);
    } else {
        ESP_LOGW(GAP_TAG, "Skip terminate: stale timer (current=%u expected=%u)",
                 (unsigned)curr, (unsigned)expected);
    }
}

// ====== Advertising / GAP ======
static bool start_advertising_now(advertising_request_t request) {
    uint32_t duration_ms = request.duration_ms;
    if (request.operation_id != s_operation_id) return false;
    if (duration_ms == 0 || !ble_hs_synced() ||
        s_link_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(GAP_TAG, "Advertising start rejected duration=%u synced=%d conn=%u",
                 (unsigned)duration_ms, (int)ble_hs_synced(),
                 (unsigned)s_link_handle);
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, 0);
        return false;
    }

    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(GAP_TAG, "ble_gap_adv_stop before restart rc=%d", rc);
            publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                          request.request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
            return false;
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
    if (pairing && pair_state_get() == PAIR_STATE_IDLE &&
        !pair_mode_prepare_session()) {
        pair_mode_deactivate();
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, 0);
        return false;
    }
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
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }
    rc = ble_gap_adv_set_data(adv_data, adv_len);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gap_adv_set_data rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
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
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }
    rc = ble_gap_adv_rsp_set_data(rsp_data, rsp_len);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gap_adv_rsp_set_data rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }

    rc = ble_gap_adv_start(g_own_addr_type, NULL, (int32_t)duration_ms,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(GAP_TAG, "ble_gap_adv_start rc=%d", rc);
        publish_event(RTEVT_BLE_START_FAILED, request.operation_id,
                      request.request_id, BLE_HS_CONN_HANDLE_NONE, (uint16_t)rc);
        return false;
    }
    s_active_adv_operation_id = request.operation_id;
    s_active_adv_request_id = request.request_id;
    ESP_LOGI(GAP_TAG, "Advertising started (%s) duration_ms=%u req=%u",
             pairing ? "PAIR" : "MAIN", (unsigned)duration_ms,
             (unsigned)request.request_id);
    return true;
}

bool gap_start_advertising(uint32_t operation_id, uint32_t request_id,
                           uint32_t duration_ms) {
    if (!s_initialized || nimble_port_get_dflt_eventq() == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_adv_mux);
    s_adv_request = (advertising_request_t){duration_ms, operation_id, request_id};
    portEXIT_CRITICAL(&s_adv_mux);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_advertise_event);
    return true;
}

bool gap_refresh_advertising(uint32_t operation_id, uint32_t request_id,
                             uint32_t duration_ms) {
    return gap_start_advertising(operation_id, request_id, duration_ms);
}

void gap_disconnect_client(uint16_t conn_handle) {
    if (!s_initialized || nimble_port_get_dflt_eventq() == NULL) {
        return;
    }
    s_disconnect_handle = conn_handle;
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

    if (s_fatal_error || s_initialized || s_controller_enabled) return false;

    if (!s_host_stopped) s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) return false;
    esp_err_t err;
    if (!s_controller_initialized) {
        esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(GAP_TAG, "controller init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_controller_initialized = true;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(GAP_TAG, "controller enable failed: %s", esp_err_to_name(err));
        return false;
    }
    s_controller_enabled = true;

    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(GAP_TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
        return false;
    }
    s_initialized = true;
    ble_npl_event_init(&s_advertise_event, advertise_cb, NULL);
    ble_npl_event_init(&s_disconnect_event, disconnect_client_cb, NULL);
    ble_npl_event_init(&s_stop_event, stop_host_cb, NULL);
    ble_npl_event_init(&s_quiesce_event, quiesce_host_cb, NULL);
    if (!setup_services()) return false;

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.reset_cb = host_reset_cb;
    if (xTaskCreate(ble_host_task, "nimble_host", NIMBLE_HS_STACK_SIZE,
                    NULL, configMAX_PRIORITIES - 4, &s_host_task) != pdPASS) {
        ESP_LOGE(GAP_TAG, "NimBLE host task allocation failed");
        return false;
    }
    xSemaphoreTake(s_metrics_mutex, portMAX_DELAY);
    s_metrics_enabled = true;
    xSemaphoreGive(s_metrics_mutex);
    log_memory("host_started", (unsigned)uxTaskGetStackHighWaterMark(s_host_task));
    ESP_LOGI(GAP_TAG, "NimBLE initialized lazily operation=%u",
             (unsigned)operation_id);
    return true;
}

void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    vTaskSuspend(NULL);
}

bool gap_ble_stop(void) {
    if (s_fatal_error) return false;
    if (xSemaphoreTake(s_metrics_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(GAP_TAG, "Telemetry still using NimBLE during stop");
        return false;
    }
    s_metrics_enabled = false;
    xSemaphoreGive(s_metrics_mutex);
    unsigned host_stack_free = 0;
    if (s_host_task) {
        if (!s_stop_pending) {
            s_stop_pending = true;
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_stop_event);
        }
        if (xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(GAP_TAG, "NimBLE host stop timed out");
            return false;
        }
        s_stop_pending = false;
        if (s_stop_status != 0) {
            ESP_LOGE(GAP_TAG, "NimBLE host stop failed rc=%d", s_stop_status);
            return false;
        }
        host_stack_free = (unsigned)uxTaskGetStackHighWaterMark(s_host_task);
        vTaskDelete(s_host_task);
        s_host_task = NULL;
    }
    if (s_initialized) {
        gatt_deinit_events();
        ble_npl_event_deinit(&s_advertise_event);
        ble_npl_event_deinit(&s_disconnect_event);
        ble_npl_event_deinit(&s_stop_event);
        ble_npl_event_deinit(&s_quiesce_event);
        esp_err_t err = esp_nimble_deinit();
        if (err != ESP_OK) {
            s_fatal_error = true;
            ESP_LOGE(GAP_TAG, "esp_nimble_deinit failed: %s", esp_err_to_name(err));
            return false;
        }
        s_initialized = false;
    }
    if (s_controller_enabled) {
        esp_err_t err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGE(GAP_TAG, "controller disable failed: %s", esp_err_to_name(err));
            return false;
        }
        s_controller_enabled = false;
    }
    if (s_link_handle != BLE_HS_CONN_HANDLE_NONE) clear_link();
    s_active_adv_operation_id = 0;
    s_active_adv_request_id = 0;
    log_memory("host_stopped", host_stack_free);
    return true;
}

// ====== GAP callbacks ======
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_link_handle = event->connect.conn_handle;
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

        case BLE_GAP_EVENT_DISCONNECT: {
            uint16_t handle = event->disconnect.conn.conn_handle;
            ESP_LOGI(GAP_TAG, "Disconnected handle=%u reason=0x%X",
                     (unsigned)event->disconnect.conn.conn_handle,
                     (unsigned)event->disconnect.reason);
            clear_link();
            runtime_event_t disconnected = runtime_event_make(RTEVT_CLIENT_DISCONNECTED, 0);
            disconnected.operation_id = s_operation_id;
            disconnected.conn_handle = handle;
            disconnected.reason = (uint16_t)event->disconnect.reason;
            disconnected.monotonic_timestamp_us = esp_timer_get_time();
            if (!event_bus_publish(&disconnected)) {
                /* A dropped terminal event must not leave the runtime in CONNECTED. */
                portENTER_CRITICAL(&s_disconnect_mux);
                s_undelivered_disconnect = disconnected;
                portEXIT_CRITICAL(&s_disconnect_mux);
            }
            return 0;
        }

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(GAP_TAG, "Advertising completed reason=%d",
                     event->adv_complete.reason);
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
    publish_event(RTEVT_BLE_READY, s_operation_id, 0,
                  BLE_HS_CONN_HANDLE_NONE, 0);
}
