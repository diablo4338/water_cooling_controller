#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAN_STATUS_VERSION 1
#define FAN_STATUS_PAYLOAD_LEN 6

typedef enum {
    FAN_STATE_IDLE = 0,
    FAN_STATE_STARTING = 1,
    FAN_STATE_RUNNING = 2,
    FAN_STATE_STALL = 3,
    FAN_STATE_IN_SERVICE = 4,
} fan_state_t;

void fan_control_init(void);
void fan_control_task(void *param);

void fan_control_get_status_payload(uint8_t *out, size_t len);
bool fan_control_override_set(uint8_t op_type, float target_rpm);
bool fan_control_override_set_output(uint8_t op_type, uint8_t control_type, float target_percent);
void fan_control_override_clear(uint8_t op_type);

#ifdef __cplusplus
}
#endif

#endif
