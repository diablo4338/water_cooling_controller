#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAN_STATUS_VERSION 1
#define FAN_STATUS_PAYLOAD_LEN 3

typedef enum {
    FAN_STATE_IDLE = 0,
    FAN_STATE_STARTING = 1,
    FAN_STATE_RUNNING = 2,
    FAN_STATE_STALL = 3,
    FAN_STATE_CALIBRATE = 4,
} fan_state_t;

void fan_control_init(void);
void fan_control_task(void *param);

void fan_control_get_status_payload(uint8_t *out, size_t len);
bool fan_control_is_calibrating(void);
bool fan_control_start_calibration(void);

#ifdef __cplusplus
}
#endif

#endif
