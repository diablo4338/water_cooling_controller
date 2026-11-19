#include "access.h"
#include "pair_mode.h"
#include "state.h"

bool can_access_pairing(void) {
    return pair_mode_is_active();
}

bool can_access_auth_nonce(uint16_t conn_handle) {
    return fsm_auth_request_allowed(conn_handle);
}

bool can_access_data(uint16_t conn_handle) {
    return fsm_auth_conn_check(conn_handle);
}
