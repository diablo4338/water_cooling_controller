#ifndef METRICS_BLE_H
#define METRICS_BLE_H

#include <stdbool.h>
#include <stdint.h>

extern uint16_t g_temp_attr_handles[4];

void metrics_ble_init(void);
void metrics_set_notify(uint16_t attr_handle, bool enabled);
void metrics_reset_notify(void);
void metrics_task(void *param);

#endif
