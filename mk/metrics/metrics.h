#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>

#define METRICS_TEMP_CHANNELS 4
#define METRICS_FAN_CHANGED_BIT (1U << 4)

void metrics_init(void);
uint8_t metrics_sample_all(void);
float metrics_get_temp(uint8_t channel);
float metrics_get_fan_speed_rpm(void);

#endif
