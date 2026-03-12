#ifndef FAN_STATUS_BLE_H
#define FAN_STATUS_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t g_fan_status_attr_handle;

void fan_status_ble_init(void);
void fan_status_set_notify(uint16_t attr_handle, bool enabled);
void fan_status_notify(uint16_t conn_handle, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif
