#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>

void metrics_init(void);
uint8_t metrics_sample_all(void);
float metrics_get_temp(uint8_t channel);

#endif
