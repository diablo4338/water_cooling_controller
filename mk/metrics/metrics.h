#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stdint.h>

#define METRICS_TEMP_CHANNELS 4
#define METRICS_FAN_CHANGED_BIT (1U << 4)

typedef struct {
    float temp_c[METRICS_TEMP_CHANNELS];
    uint8_t temp_valid[METRICS_TEMP_CHANNELS];
    float fan_rpm;
    uint8_t fan_valid;
} metrics_snapshot_t;

void metrics_init(void);
uint8_t metrics_sample_all(void);
float metrics_get_temp(uint8_t channel);
float metrics_get_fan_speed_rpm(void);
void metrics_get_snapshot(metrics_snapshot_t *out);
bool metrics_has_error(void);

#endif
