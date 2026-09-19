#ifndef LOAD_METRICS_BLE_H
#define LOAD_METRICS_BLE_H

#include <stdbool.h>
#include <stdint.h>

extern uint16_t g_load_state_attr_handle;
extern uint16_t g_button_state_attr_handle;
extern uint16_t g_temperature_attr_handle;
extern uint16_t g_voltage_attr_handle;
extern uint16_t g_current_attr_handle;

void load_metrics_set_notify(uint16_t attr_handle, bool enabled);
void load_metrics_reset_notify(void);
void load_metrics_publish(void);

#endif
