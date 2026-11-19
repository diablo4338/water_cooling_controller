#ifndef FAN_STATUS_BLE_H
#define FAN_STATUS_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t g_fan_status_attr_handle;
extern uint16_t g_fan_mode_status_attr_handle;
extern uint16_t g_output_limit_attr_handle;
extern uint16_t g_output_range_attr_handle;

void fan_status_ble_init(void);
void fan_status_set_notify(uint16_t attr_handle, bool enabled);
void fan_status_reset_notify(void);
void fan_status_notify(uint16_t conn_handle, const uint8_t *payload, size_t len);
size_t output_limit_status_encode(bool extended, uint8_t *payload);
void fan_runtime_status_publish(void);

#ifdef __cplusplus
}
#endif

#endif
