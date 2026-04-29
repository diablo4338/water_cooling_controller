#ifndef METRICS_BLE_H
#define METRICS_BLE_H

#include <stdbool.h>
#include <stdint.h>

#include "metrics.h"

extern uint16_t g_temp_attr_handles[METRICS_TEMP_CHANNELS];
extern uint16_t g_fan_attr_handles[METRICS_FAN_CHANNELS];
extern uint16_t g_voltage_attr_handle;
extern uint16_t g_current_attr_handle;

void metrics_ble_init(void);
void metrics_set_notify(uint16_t attr_handle, bool enabled);
void metrics_reset_notify(void);
void metrics_task(void *param);

#endif
