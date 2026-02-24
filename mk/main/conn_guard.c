#include "conn_guard.h"
#include "state.h"

bool pairing_conn_bind_or_check(uint16_t conn_handle) {
    bool ok;
    state_lock();
    if (g_pair_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        g_pair_conn_handle = conn_handle;
    }
    ok = (g_pair_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

bool pairing_conn_check(uint16_t conn_handle) {
    bool ok;
    state_lock();
    ok = (g_pair_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

bool auth_conn_check_or_any(uint16_t conn_handle) {
    bool ok;
    state_lock();
    ok = (g_auth_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
          g_auth_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

bool auth_conn_check(uint16_t conn_handle) {
    bool ok;
    state_lock();
    ok = (g_auth_conn_handle == conn_handle);
    state_unlock();
    return ok;
}

void conn_guard_reset(void) {
    state_lock();
    g_pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_auth_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    state_unlock();
}
