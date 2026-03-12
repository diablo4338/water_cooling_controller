#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAN_STATUS_VERSION 1
#define FAN_STATUS_PAYLOAD_LEN 2

typedef enum {
    FAN_STATE_IDLE = 0,
    FAN_STATE_STARTING = 1,
    FAN_STATE_RUNNING = 2,
    FAN_STATE_STALL = 3,
} fan_state_t;

void fan_control_init(void);
void fan_control_task(void *param);

void fan_control_get_status_payload(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
