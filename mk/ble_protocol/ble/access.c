#include "access.h"
#include "state.h"

bool can_access_pairing(void) {
    return fsm_is_pairing();
}

bool can_access_auth_nonce(void) {
    return fsm_is_paired();
}

bool can_access_data(void) {
    return fsm_is_authed();
}
