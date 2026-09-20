#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define RUNTIME_CONFIG_VERSION 1U
#define RUNTIME_CONFIG_WIRE_SIZE 19U
#define RUNTIME_CONFIG_RESULT_SIZE 5U

typedef struct {
    uint16_t idle_current_ma;
    uint16_t voltage_min_mv;
    uint16_t voltage_max_mv;
    uint32_t idle_timeout_s;
    int16_t button_press_deci_c;
    int16_t button_release_deci_c;
    uint16_t deep_sleep_ms;
} runtime_config_t;

typedef enum {
    CONFIG_STATUS_OK = 0,
    CONFIG_STATUS_FORMAT = 1,
    CONFIG_STATUS_RANGE = 2,
    CONFIG_STATUS_BUTTON_ORDER = 3,
    CONFIG_STATUS_VOLTAGE_ORDER = 4,
    CONFIG_STATUS_FLASH = 5,
} runtime_config_status_t;

typedef enum {
    CONFIG_FIELD_NONE = 0,
    CONFIG_FIELD_IDLE_CURRENT = 1,
    CONFIG_FIELD_VOLTAGE_MIN = 2,
    CONFIG_FIELD_VOLTAGE_MAX = 3,
    CONFIG_FIELD_IDLE_TIMEOUT = 4,
    CONFIG_FIELD_BUTTON_PRESS = 5,
    CONFIG_FIELD_BUTTON_RELEASE = 6,
    CONFIG_FIELD_DEEP_SLEEP = 7,
} runtime_config_field_t;

typedef struct {
    uint16_t request_id;
    runtime_config_status_t status;
    runtime_config_field_t field;
} runtime_config_result_t;

void runtime_config_init(void);
void runtime_config_get(runtime_config_t *out);
void runtime_config_encode(uint8_t out[RUNTIME_CONFIG_WIRE_SIZE], uint16_t request_id);
void runtime_config_result_encode(const runtime_config_result_t *result,
                                  uint8_t out[RUNTIME_CONFIG_RESULT_SIZE]);
void runtime_config_apply_wire(const uint8_t *data, size_t length,
                               runtime_config_result_t *result);

#endif
