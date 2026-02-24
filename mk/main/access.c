#include "access.h"
#include "state.h"

bool can_access_pairing(void) {
    bool ok;
    state_lock();
    ok = g_pairing_mode && !g_paired;
    state_unlock();
    return ok;
}

bool can_access_auth_nonce(void) {
    bool ok;
    state_lock();
    ok = g_paired;
    state_unlock();
    return ok;
}

bool can_access_data(void) {
    bool ok;
    state_lock();
    ok = g_authed;
    state_unlock();
    return ok;
}
