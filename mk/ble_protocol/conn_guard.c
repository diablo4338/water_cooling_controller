#include "conn_guard.h"
#include "state.h"

bool pairing_conn_bind_or_check(uint16_t conn_handle) {
    return fsm_pair_conn_bind_or_check(conn_handle);
}

bool pairing_conn_check(uint16_t conn_handle) {
    return fsm_pair_conn_check(conn_handle);
}

bool auth_conn_check_or_any(uint16_t conn_handle) {
    return fsm_auth_conn_check_or_any(conn_handle);
}

bool auth_conn_check(uint16_t conn_handle) {
    return fsm_auth_conn_check(conn_handle);
}

void conn_guard_reset(void) {
    fsm_conn_guard_reset();
}
