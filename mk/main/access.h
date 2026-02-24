#ifndef ACCESS_H
#define ACCESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool can_access_pairing(void);
bool can_access_auth_nonce(void);
bool can_access_data(void);

#ifdef __cplusplus
}
#endif

#endif
