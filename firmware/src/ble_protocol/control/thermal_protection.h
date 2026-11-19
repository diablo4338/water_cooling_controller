#pragma once

#include <stdbool.h>

typedef enum {
    THERMAL_INPUT_READY = 0,
    THERMAL_INPUT_ADC_INITIALIZING,
    THERMAL_INPUT_ADC_OFFLINE,
    THERMAL_INPUT_NTC_UNAVAILABLE,
} thermal_input_state_t;

thermal_input_state_t thermal_protection_get_input(void);
typedef struct {
    thermal_input_state_t input;
    float temperature;
} thermal_observation_t;

thermal_observation_t thermal_protection_update(bool temperature_control, float threshold);
