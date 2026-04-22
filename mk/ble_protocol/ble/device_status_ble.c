#include "device_status_ble.h"

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "access.h"
#include "conn_guard.h"
#include "state.h"

uint16_t g_device_status_attr_handle = 0;
static bool g_device_status_notify_enabled = false;

void device_status_ble_init(void) {
    g_device_status_attr_handle = 0;
    g_device_status_notify_enabled = false;
}

void device_status_set_notify(uint16_t attr_handle, bool enabled) {
    if (attr_handle == 0) return;
    if (attr_handle != g_device_status_attr_handle) return;
    g_device_status_notify_enabled = enabled;
}

void device_status_reset_notify(void) {
    g_device_status_notify_enabled = false;
}

void device_status_notify(uint16_t conn_handle, const uint8_t *payload, size_t len) {
    if (!g_device_status_notify_enabled) return;
    if (g_device_status_attr_handle == 0) return;
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!auth_conn_check(conn_handle)) return;
    if (!can_access_data()) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, g_device_status_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify device status rc=%d", rc);
    }
}
