#ifndef ACCESS_H
#define ACCESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool can_access_pairing(void);
bool can_access_auth_nonce(uint16_t conn_handle);
bool can_access_data(uint16_t conn_handle);

#ifdef __cplusplus
}
#endif

#endif
