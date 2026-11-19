#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OUTPUT_LIMIT_VERSION 1
#define OUTPUT_LIMIT_STATUS_VERSION 2
#define OUTPUT_LIMIT_PAYLOAD_LEN 6
#define OUTPUT_LIMIT_EXTENDED_VERSION 3
#define OUTPUT_LIMIT_EXTENDED_LEN 8
#define OUTPUT_LIMIT_NO_TEMPORARY_MINIMUM UINT8_MAX
#define OUTPUT_LIMIT_WRITE_MAX 1
#define OUTPUT_LIMIT_RESET 2

typedef enum {
    OUTPUT_LIMIT_NORMAL = 0,
    OUTPUT_LIMIT_MEASURE,
    OUTPUT_LIMIT_VERIFY_FORMULA,
    OUTPUT_LIMIT_VERIFY_STEP,
    OUTPUT_LIMIT_VERIFY_MINIMUM, // Legacy BLE state ID.
    OUTPUT_LIMIT_LIMITED,
    OUTPUT_LIMIT_MINIMUM,
    OUTPUT_LIMIT_SENSOR_ERROR,
    OUTPUT_LIMIT_FAULT,
    OUTPUT_LIMIT_CONFIRMING, // Legacy BLE state ID.
    OUTPUT_LIMIT_START_BLOCKED, // Legacy BLE state ID.
    OUTPUT_LIMIT_VERIFY_TIMEOUT,
    OUTPUT_LIMIT_WAIT_CONFIG,
    OUTPUT_LIMIT_SEARCH_UP,
} output_limit_state_t;

typedef enum {
    OUTPUT_LIMIT_RESET_NONE = 0,
    OUTPUT_LIMIT_RESET_PENDING,
    OUTPUT_LIMIT_RESET_APPLIED,
    OUTPUT_LIMIT_RESET_REJECTED,
} output_limit_reset_result_t;

typedef struct {
    output_limit_state_t state;
    uint8_t configured;
    uint8_t effective;
    uint8_t confirmed_effective;
    uint8_t temporary_minimum;
    bool alert_without_load;
    bool verified_output;
    uint8_t reset_sequence;
    output_limit_reset_result_t reset_result;
} output_limit_status_t;

typedef enum {
    OUTPUT_LIMIT_MODE_CONTINUOUS,
    OUTPUT_LIMIT_MODE_TEMPERATURE,
    OUTPUT_LIMIT_MODE_SERVICE,
} output_limit_mode_t;

typedef struct {
    float percent;
    uint8_t control_type;
} output_limit_command_t;

output_limit_command_t output_limit_step(float requested, float actual, uint8_t control_type,
                                         uint8_t maximum, float minimum, output_limit_mode_t mode, int64_t now);
void output_limit_output_applied(float percent, uint8_t control_type, int64_t now);
output_limit_status_t output_limit_get_status(void);
bool output_limit_is_adjusting(output_limit_state_t state);
bool output_limit_settings_locked(void);
bool output_limit_begin_config_write(void);
void output_limit_end_config_write(void);
// Called by the fan task before applying its next command.
void output_limit_request_stop(void);
bool output_limit_request_reset(void);
