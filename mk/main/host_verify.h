#ifndef HOST_VERIFY_H
#define HOST_VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void host_verify_update(const uint8_t host_pub[65]);
bool host_verify_check(void);

#ifdef __cplusplus
}
#endif

#endif
