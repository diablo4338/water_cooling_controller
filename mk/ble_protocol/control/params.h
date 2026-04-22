#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARAMS_VERSION 4
#define PARAMS_PAYLOAD_LEN 21
#define PARAMS_STATUS_LEN 3

#define PARAM_STATUS_OK 0x00
#define PARAM_STATUS_INVALID 0x01
#define PARAM_STATUS_BUSY 0x02

#define PARAM_FIELD_FAN_MIN_SPEED 0
#define PARAM_FIELD_FAN_CONTROL_TYPE 1
#define PARAM_FIELD_FAN_MAX_TEMP 2
#define PARAM_FIELD_FAN_OFF_DELTA 3
#define PARAM_FIELD_FAN_START_TEMP 4
#define PARAM_FIELD_FAN_MODE 5
#define PARAM_FIELD_FAN_MONITORING_ENABLED 6
#define PARAM_FIELD_NONE 0xFF

#define PARAM_MASK_FAN_MIN_SPEED (1u << 0)
#define PARAM_MASK_FAN_CONTROL_TYPE (1u << 1)
#define PARAM_MASK_FAN_MAX_TEMP (1u << 2)
#define PARAM_MASK_FAN_OFF_DELTA (1u << 3)
#define PARAM_MASK_FAN_START_TEMP (1u << 4)
#define PARAM_MASK_FAN_MODE (1u << 5)
#define PARAM_MASK_FAN_MONITORING_ENABLED (1u << 6)
#define PARAM_MASK_ALL (PARAM_MASK_FAN_MIN_SPEED | PARAM_MASK_FAN_CONTROL_TYPE | PARAM_MASK_FAN_MAX_TEMP | \
                        PARAM_MASK_FAN_OFF_DELTA | PARAM_MASK_FAN_START_TEMP | PARAM_MASK_FAN_MODE | \
                        PARAM_MASK_FAN_MONITORING_ENABLED)

#define PARAM_FAN_CONTROL_DC 0
#define PARAM_FAN_CONTROL_PWM 1
#define PARAM_FAN_MODE_CONTINUOUS 0
#define PARAM_FAN_MODE_TEMP_SENSOR 1
#define PARAM_FAN_MODE_INACTIVE 2

typedef struct {
    int32_t fan_min_speed;
    uint8_t fan_control_type;
    int32_t fan_max_temp;
    int32_t fan_off_delta;
    int32_t fan_start_temp;
    uint8_t fan_mode;
    uint8_t fan_monitoring_enabled;
} params_t;

void params_init(void);

bool params_read(params_t *out);
bool params_write(const params_t *params, uint8_t mask);
bool params_cache_get(params_t *out);

bool params_set_pending_payload(const uint8_t *data, size_t len);
bool params_get_current_payload(uint8_t *out, size_t len);
void params_get_last_status_payload(uint8_t *out, size_t len);
void params_set_last_status(uint8_t status, uint8_t field);
uint8_t params_apply(uint8_t *field_id);

#ifdef __cplusplus
}
#endif

#endif
