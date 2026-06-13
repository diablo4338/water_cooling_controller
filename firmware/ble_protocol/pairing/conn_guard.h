#ifndef CONN_GUARD_H
#define CONN_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pairing_conn_bind_or_check(uint16_t conn_handle);
bool pairing_conn_check(uint16_t conn_handle);

bool auth_conn_check_or_any(uint16_t conn_handle);
bool auth_conn_check(uint16_t conn_handle);

void conn_guard_reset(void);

#ifdef __cplusplus
}
#endif

#endif
