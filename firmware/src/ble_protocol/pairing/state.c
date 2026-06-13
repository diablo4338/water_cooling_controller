#include "state.h"

const char *TAG = "pair-stack";

typedef struct {
    fsm_state_t state;
    uint16_t conn_handle;
    uint16_t term_conn_handle;
    uint16_t pair_conn_handle;
    uint16_t auth_conn_handle;
} fsm_ctx_t;

static fsm_ctx_t g_fsm = {
    .state = FSM_STATE_UNPAIRED_IDLE,
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .term_conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .pair_conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .auth_conn_handle = BLE_HS_CONN_HANDLE_NONE,
};

uint8_t g_own_addr_type;

uint8_t dev_nonce[16];
uint8_t auth_nonce[16];

uint8_t dev_pub65[65];
uint8_t host_pub65[65];

uint8_t K[32];
uint8_t host_id_hash[32];

esp_timer_handle_t g_term_timer;
esp_timer_handle_t g_pair_timer;

portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

void state_lock(void) {
    portENTER_CRITICAL(&g_state_mux);
}

void state_unlock(void) {
    portEXIT_CRITICAL(&g_state_mux);
}

static void fsm_reset_locked(void) {
    g_fsm.state = FSM_STATE_UNPAIRED_IDLE;
    g_fsm.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_fsm.term_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_fsm.pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_fsm.auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

void fsm_reset(void) {
    state_lock();
    fsm_reset_locked();
    state_unlock();
}

bool fsm_dispatch(fsm_event_t event, uint16_t conn_handle) {
    bool handled = false;

    state_lock();
    if (event == FSM_EVT_TRUST_RESET) {
        fsm_reset_locked();
        state_unlock();
        return true;
    }

    switch (event) {
        case FSM_EVT_CONNECT:
            g_fsm.conn_handle = conn_handle;
            g_fsm.term_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_fsm.auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (g_fsm.state == FSM_STATE_AUTHED) {
                g_fsm.state = FSM_STATE_PAIRED_UNAUTH;
            }
            handled = true;
            break;
        case FSM_EVT_DISCONNECT:
            g_fsm.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_fsm.term_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_fsm.auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_fsm.pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (g_fsm.state == FSM_STATE_AUTHED) {
                g_fsm.state = FSM_STATE_PAIRED_UNAUTH;
            }
            handled = true;
            break;
        default:
            break;
    }

    if (!handled) {
        switch (g_fsm.state) {
            case FSM_STATE_UNPAIRED_IDLE:
                if (event == FSM_EVT_TRUST_LOADED) {
                    g_fsm.state = FSM_STATE_PAIRED_UNAUTH;
                    handled = true;
                }
                break;
            case FSM_STATE_PAIRED_UNAUTH:
                if (event == FSM_EVT_AUTH_OK) {
                    g_fsm.state = FSM_STATE_AUTHED;
                    g_fsm.auth_conn_handle = conn_handle;
                    handled = true;
                } else if (event == FSM_EVT_TRUST_LOADED) {
                    handled = true;
                } else if (event == FSM_EVT_AUTH_FAILED) {
                    handled = true;
                }
                break;
            case FSM_STATE_AUTHED:
                if (event == FSM_EVT_AUTH_FAILED) {
                    g_fsm.state = FSM_STATE_PAIRED_UNAUTH;
                    handled = true;
                } else if (event == FSM_EVT_TRUST_LOADED) {
                    handled = true;
                }
                break;
            default:
                break;
        }
    }

    state_unlock();
    return handled;
}

fsm_state_t fsm_get_state(void) {
    fsm_state_t state;
    state_lock();
    state = g_fsm.state;
    state_unlock();
    return state;
}

bool fsm_is_paired(void) {
    fsm_state_t state = fsm_get_state();
    return state == FSM_STATE_PAIRED_UNAUTH || state == FSM_STATE_AUTHED;
}

bool fsm_is_authed(void) {
    return fsm_get_state() == FSM_STATE_AUTHED;
}

uint16_t fsm_get_conn_handle(void) {
    uint16_t handle;
    state_lock();
    handle = g_fsm.conn_handle;
    state_unlock();
    return handle;
}

uint16_t fsm_get_term_conn_handle(void) {
    uint16_t handle;
    state_lock();
    handle = g_fsm.term_conn_handle;
    state_unlock();
    return handle;
}

uint16_t fsm_get_pair_conn_handle(void) {
    uint16_t handle;
    state_lock();
    handle = g_fsm.pair_conn_handle;
    state_unlock();
    return handle;
}

uint16_t fsm_get_auth_conn_handle(void) {
    uint16_t handle;
    state_lock();
    handle = g_fsm.auth_conn_handle;
    state_unlock();
    return handle;
}

bool fsm_pair_conn_bind_or_check(uint16_t conn_handle) {
    bool ok;
    state_lock();
    if (g_fsm.pair_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        g_fsm.pair_conn_handle = conn_handle;
    }
    ok = (g_fsm.pair_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

bool fsm_pair_conn_check(uint16_t conn_handle) {
    bool ok;
    state_lock();
    ok = (g_fsm.pair_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

bool fsm_auth_conn_check_or_any(uint16_t conn_handle) {
    bool ok;
    state_lock();
    ok = (g_fsm.auth_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
          g_fsm.auth_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

bool fsm_auth_conn_check(uint16_t conn_handle) {
    bool ok;
    state_lock();
    ok = (g_fsm.auth_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

void fsm_set_term_conn_handle(uint16_t conn_handle) {
    state_lock();
    g_fsm.term_conn_handle = conn_handle;
    state_unlock();
}

void fsm_conn_guard_reset(void) {
    state_lock();
    g_fsm.pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_fsm.auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    state_unlock();
}
