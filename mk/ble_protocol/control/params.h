#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARAMS_VERSION 2
#define PARAMS_PAYLOAD_LEN 19
#define PARAMS_STATUS_LEN 3

#define PARAM_STATUS_OK 0x00
#define PARAM_STATUS_INVALID 0x01
#define PARAM_STATUS_BUSY 0x02

#define PARAM_FIELD_TARGET_TEMP 0
#define PARAM_FIELD_FAN_MIN_RPM 1
#define PARAM_FIELD_ALARM_DELTA 2
#define PARAM_FIELD_FAN_MIN_SPEED 3
#define PARAM_FIELD_FAN_CONTROL_TYPE 4
#define PARAM_FIELD_NONE 0xFF

#define PARAM_MASK_TARGET_TEMP (1u << 0)
#define PARAM_MASK_FAN_MIN_RPM (1u << 1)
#define PARAM_MASK_ALARM_DELTA (1u << 2)
#define PARAM_MASK_FAN_MIN_SPEED (1u << 3)
#define PARAM_MASK_FAN_CONTROL_TYPE (1u << 4)
#define PARAM_MASK_ALL (PARAM_MASK_TARGET_TEMP | PARAM_MASK_FAN_MIN_RPM | PARAM_MASK_ALARM_DELTA | \
                        PARAM_MASK_FAN_MIN_SPEED | PARAM_MASK_FAN_CONTROL_TYPE)

#define PARAM_FAN_CONTROL_DC 0
#define PARAM_FAN_CONTROL_PWM 1

typedef struct {
    float target_temp_c;
    float fan_min_rpm;
    float alarm_delta_c;
    int32_t fan_min_speed;
    uint8_t fan_control_type;
} params_t;

void params_init(void);

bool params_read(params_t *out);
bool params_write(const params_t *params, uint8_t mask);

bool params_set_pending_payload(const uint8_t *data, size_t len);
bool params_get_current_payload(uint8_t *out, size_t len);
void params_get_last_status_payload(uint8_t *out, size_t len);
void params_set_last_status(uint8_t status, uint8_t field);
uint8_t params_apply(uint8_t *field_id);

#ifdef __cplusplus
}
#endif

#endif
