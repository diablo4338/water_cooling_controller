#include <string.h>

#include "crypto.h"
#include "host_verify.h"
#include "state.h"

void host_verify_update(const uint8_t host_pub[65]) {
    uint8_t tmp[32];
    if (sha256(host_pub, 65, tmp) != 0) return;
    state_lock();
    memcpy(host_id_hash, tmp, sizeof(host_id_hash));
    state_unlock();
}

bool host_verify_check(void) {
    uint8_t tmp[32];
    uint8_t stored[32];
    if (sha256(host_pub65, 65, tmp) != 0) return false;

    state_lock();
    memcpy(stored, host_id_hash, sizeof(stored));
    state_unlock();

    return memcmp(tmp, stored, sizeof(stored)) == 0;
}
