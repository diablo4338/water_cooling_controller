#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stdint.h>

#define METRICS_TEMP_CHANNELS 4
#define METRICS_FAN_CHANNELS 4
#define METRICS_TACH_CHANNEL_WINDOW_MS 800
#define METRICS_TACH_SCAN_US (METRICS_FAN_CHANNELS * METRICS_TACH_CHANNEL_WINDOW_MS * 1000LL)
#define METRICS_CONTROL_TEMP_CHANNEL 3
#define METRICS_FAN_CHANGED_BIT(channel) (1U << (METRICS_TEMP_CHANNELS + (channel)))
#define METRICS_FAN_CHANGED_MASK (0x0FU << METRICS_TEMP_CHANNELS)

typedef struct {
    float temp_c[METRICS_TEMP_CHANNELS];
    uint8_t temp_valid[METRICS_TEMP_CHANNELS];
    float fan_rpm[METRICS_FAN_CHANNELS];
    uint8_t fan_valid[METRICS_FAN_CHANNELS];
    float voltage_v;
    float current_ma;
    uint8_t power_valid;
} metrics_snapshot_t;

typedef struct {
    int64_t started_us;
    int64_t completed_us;
    float rpm;  // NAN when the window contained no valid period.
} metrics_fan_observation_t;

typedef enum {
    METRICS_ADC_UNKNOWN = 0,
    METRICS_ADC_AVAILABLE,
    METRICS_ADC_OFFLINE,
    METRICS_ADC_RECOVERING,
} metrics_adc_state_t;

metrics_adc_state_t metrics_get_adc_state(void);
void metrics_init(void);
uint16_t metrics_sample_all(void);
float metrics_get_temp(uint8_t channel);
float metrics_get_fan_speed_rpm_channel(uint8_t channel);
bool metrics_get_fan_observation(uint8_t channel, metrics_fan_observation_t *out);
float metrics_get_voltage_v(void);
float metrics_get_current_ma(void);
void metrics_get_snapshot(metrics_snapshot_t *out);
bool metrics_get_temp_sample(uint8_t channel, float *temp_c, int64_t *sampled_us);
bool metrics_has_error(void);
bool metrics_has_ads_error(void);
bool metrics_has_ina_error(void);

#endif
