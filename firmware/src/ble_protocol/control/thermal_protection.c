#include "thermal_protection.h"

#include <math.h>

#include "device_status.h"
#include "metrics.h"
#include "esp_timer.h"

#define THERMAL_SAMPLE_MAX_AGE_US 3000000LL
#define THERMAL_EMERGENCY_THRESHOLD_C 50.0f

static thermal_input_state_t thermal_protection_read_input(float *temp) {
    metrics_adc_state_t adc = metrics_get_adc_state();
    if (adc == METRICS_ADC_UNKNOWN) return THERMAL_INPUT_ADC_INITIALIZING;
    if (adc != METRICS_ADC_AVAILABLE) return THERMAL_INPUT_ADC_OFFLINE;

    int64_t sampled;
    if (!metrics_get_temp_sample(METRICS_CONTROL_TEMP_CHANNEL, temp, &sampled) ||
        !isfinite(*temp) || esp_timer_get_time() - sampled > THERMAL_SAMPLE_MAX_AGE_US) {
        return THERMAL_INPUT_NTC_UNAVAILABLE;
    }
    return THERMAL_INPUT_READY;
}

thermal_input_state_t thermal_protection_get_input(void) {
    float temp;
    return thermal_protection_read_input(&temp);
}

thermal_observation_t thermal_protection_update(bool temperature_control, float threshold) {
    float temp = NAN;
    thermal_input_state_t input = thermal_protection_read_input(&temp);
    bool overheat = input == THERMAL_INPUT_READY &&
                    (temp > THERMAL_EMERGENCY_THRESHOLD_C || (temperature_control && temp >= threshold));
    device_error_mask_t active = overheat ? DEVICE_ERROR_OVERHEAT : DEVICE_ERROR_NONE;
    if (input == THERMAL_INPUT_ADC_OFFLINE) active |= DEVICE_ERROR_ADC_OFFLINE;
    if (input == THERMAL_INPUT_NTC_UNAVAILABLE) active |= DEVICE_ERROR_NTC_DISCONNECTED;
    device_status_update_error_flags(
        DEVICE_ERROR_OVERHEAT | DEVICE_ERROR_ADC_OFFLINE | DEVICE_ERROR_NTC_DISCONNECTED, active
    );
    return (thermal_observation_t){.input = input, .temperature = input == THERMAL_INPUT_READY ? temp : NAN};
}
