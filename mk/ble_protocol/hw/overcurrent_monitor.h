#ifndef OVERCURRENT_MONITOR_H
#define OVERCURRENT_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void overcurrent_monitor_init(void);
bool overcurrent_monitor_alert_active(void);
bool overcurrent_monitor_latched_active(void);
bool overcurrent_monitor_read_alert_status(uint16_t *mask_enable);
void overcurrent_monitor_clear_latched(void);

#ifdef __cplusplus
}
#endif

#endif
