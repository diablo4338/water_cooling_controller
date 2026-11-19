#include "pair_mode.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"

#include "crypto.h"
#include "ecdh.h"
#include "gap.h"
#include "pair_state.h"
#include "state.h"

#define PAIR_MODE_TIMEOUT_US (60LL * 1000LL * 1000LL)

typedef enum {
    PAIR_WINDOW_CLOSED,
    PAIR_WINDOW_OPEN,
} pair_window_state_t;

static pair_window_state_t g_window_state = PAIR_WINDOW_CLOSED;
static int64_t g_deadline_us;
static esp_timer_handle_t g_pair_timer;
static struct ble_npl_event g_open_event;
static struct ble_npl_event g_timeout_event;

static void clear_pair_session_buffers(void) {
    state_lock();
    memset(dev_nonce, 0, sizeof(dev_nonce));
    memset(dev_pub65, 0, sizeof(dev_pub65));
    memset(host_pub65, 0, sizeof(host_pub65));
    memset(K, 0, sizeof(K));
    memset(host_id_hash, 0, sizeof(host_id_hash));
    state_unlock();
}

static bool prepare_session(void) {
    pair_state_dispatch(PAIR_EVT_SESSION_RESET);
    clear_pair_session_buffers();
    rand_bytes(dev_nonce, sizeof(dev_nonce));
    if (ecdh_make_dev_keys() != 0) {
        ESP_LOGE(TAG, "ECDH keygen failed");
        clear_pair_session_buffers();
        return false;
    }
    return pair_state_dispatch(PAIR_EVT_SESSION_STARTED);
}

static void close_window(void) {
    state_lock();
    g_window_state = PAIR_WINDOW_CLOSED;
    g_deadline_us = 0;
    state_unlock();

    esp_timer_stop(g_pair_timer);
    pair_state_dispatch(PAIR_EVT_SESSION_RESET);
    clear_pair_session_buffers();
    ESP_LOGW(TAG, "Pairing window closed");
}

static void restart_advertising(void) {
    stop_advertising();
    start_advertising();
}

static void disconnect_client(uint16_t conn_handle) {
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
}

static void on_open_request(struct ble_npl_event *event) {
    (void)event;
    pair_mode_dispatch(PAIR_WINDOW_OPEN_REQUEST);
}

static void on_timeout_event(struct ble_npl_event *event) {
    (void)event;
    pair_mode_dispatch(PAIR_WINDOW_EXPIRED);
}

static void post_timeout(void *arg) {
    (void)arg;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g_timeout_event);
}

void pair_mode_init(void) {
    pair_state_dispatch(PAIR_EVT_SESSION_RESET);
    clear_pair_session_buffers();
    ble_npl_event_init(&g_open_event, on_open_request, NULL);
    ble_npl_event_init(&g_timeout_event, on_timeout_event, NULL);
    const esp_timer_create_args_t args = {
        .callback = post_timeout,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pair_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &g_pair_timer));
}

void pair_mode_request_open(void) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g_open_event);
}

bool pair_mode_is_active(void) {
    state_lock();
    bool active = g_window_state == PAIR_WINDOW_OPEN && esp_timer_get_time() < g_deadline_us;
    state_unlock();
    return active;
}

void pair_mode_dispatch(pair_window_event_t event) {
    switch (event) {
        case PAIR_WINDOW_OPEN_REQUEST: {
            esp_timer_stop(g_pair_timer);
            uint16_t conn_handle = fsm_get_conn_handle();
            if (prepare_session()) {
                int64_t deadline_us = esp_timer_get_time() + PAIR_MODE_TIMEOUT_US;
                esp_err_t err = esp_timer_start_once(g_pair_timer, PAIR_MODE_TIMEOUT_US);
                if (err == ESP_OK) {
                    state_lock();
                    g_deadline_us = deadline_us;
                    g_window_state = PAIR_WINDOW_OPEN;
                    state_unlock();
                    ESP_LOGW(TAG, "Pairing window open (60s)");
                    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                        disconnect_client(conn_handle);
                    } else {
                        restart_advertising();
                    }
                    return;
                }
                ESP_LOGE(TAG, "Pairing timer start failed: %s", esp_err_to_name(err));
            }
            close_window();
            if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                restart_advertising();
            }
            break;
        }
        case PAIR_WINDOW_EXPIRED: {
            // A queued timeout can outlive the window that posted it.
            if (g_window_state == PAIR_WINDOW_CLOSED || pair_mode_is_active()) {
                return;
            }
            close_window();
            uint16_t conn_handle = fsm_get_conn_handle();
            if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                restart_advertising();
            } else if (!fsm_is_authed()) {
                disconnect_client(conn_handle);
            }
            break;
        }
        case PAIR_WINDOW_LINK_CLOSED:
            if (g_window_state == PAIR_WINDOW_OPEN &&
                (!pair_mode_is_active() || !prepare_session() || !pair_mode_is_active())) {
                close_window();
            }
            break;
        case PAIR_WINDOW_PAIR_COMPLETED:
        case PAIR_WINDOW_FACTORY_RESET:
            if (g_window_state == PAIR_WINDOW_OPEN) {
                close_window();
            }
            break;
    }
}
