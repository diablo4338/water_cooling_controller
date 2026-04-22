#ifndef DEVICE_STATUS_BLE_H
#define DEVICE_STATUS_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t g_device_status_attr_handle;

void device_status_ble_init(void);
void device_status_set_notify(uint16_t attr_handle, bool enabled);
void device_status_reset_notify(void);
void device_status_notify(uint16_t conn_handle, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif
