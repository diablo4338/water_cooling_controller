#include "access.h"
#include "pair_mode.h"
#include "state.h"

bool can_access_pairing(void) {
    return pair_mode_is_active();
}

bool can_access_auth_nonce(void) {
    return fsm_is_paired();
}

bool can_access_data(void) {
    return fsm_is_authed();
}
