#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stdint.h>

#define METRICS_TEMP_CHANNELS 4
#define METRICS_FAN_CHANNELS 4
#define METRICS_FAN_CHANGED_BIT(channel) (1U << (METRICS_TEMP_CHANNELS + (channel)))
#define METRICS_FAN_CHANGED_MASK (0x0FU << METRICS_TEMP_CHANNELS)
#define METRICS_VOLTAGE_CHANGED_BIT (1U << (METRICS_TEMP_CHANNELS + METRICS_FAN_CHANNELS))
#define METRICS_CURRENT_CHANGED_BIT (1U << (METRICS_TEMP_CHANNELS + METRICS_FAN_CHANNELS + 1))

typedef struct {
    float temp_c[METRICS_TEMP_CHANNELS];
    uint8_t temp_valid[METRICS_TEMP_CHANNELS];
    float fan_rpm[METRICS_FAN_CHANNELS];
    uint8_t fan_valid[METRICS_FAN_CHANNELS];
    float voltage_v;
    float current_ma;
    uint8_t power_valid;
} metrics_snapshot_t;

void metrics_init(void);
uint16_t metrics_sample_all(void);
float metrics_get_temp(uint8_t channel);
float metrics_get_fan_speed_rpm_channel(uint8_t channel);
float metrics_get_voltage_v(void);
float metrics_get_current_ma(void);
void metrics_get_snapshot(metrics_snapshot_t *out);
bool metrics_has_error(void);
bool metrics_has_ads_error(void);
bool metrics_has_ina_error(void);

#endif
