#include "load_metrics_ble.h"

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "access.h"
#include "load_runtime.h"
#include "state.h"

uint16_t g_load_state_attr_handle;
uint16_t g_button_state_attr_handle;
uint16_t g_temperature_attr_handle;
uint16_t g_voltage_attr_handle;
uint16_t g_current_attr_handle;

static bool state_notify;
static bool button_notify;
static bool temperature_notify;
static bool voltage_notify;
static bool current_notify;

void load_metrics_set_notify(uint16_t attr_handle, bool enabled) {
    if (attr_handle == g_load_state_attr_handle) state_notify = enabled;
    else if (attr_handle == g_button_state_attr_handle) button_notify = enabled;
    else if (attr_handle == g_temperature_attr_handle) temperature_notify = enabled;
    else if (attr_handle == g_voltage_attr_handle) voltage_notify = enabled;
    else if (attr_handle == g_current_attr_handle) current_notify = enabled;
}

void load_metrics_reset_notify(void) {
    state_notify = false;
    button_notify = false;
    temperature_notify = false;
    voltage_notify = false;
    current_notify = false;
}

static void notify_value(uint16_t conn, uint16_t handle, bool enabled,
                         const void *value, uint16_t length) {
    if (!enabled || handle == 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, length);
    if (om) ble_gatts_notify_custom(conn, handle, om);
}

void load_metrics_publish(void) {
    uint16_t conn = fsm_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE || !fsm_is_authed() || !can_access_data()) return;

    load_runtime_snapshot_t snapshot;
    load_runtime_get_snapshot(&snapshot);
    notify_value(conn, g_load_state_attr_handle, state_notify,
                 &snapshot.load_state, sizeof(snapshot.load_state));
    notify_value(conn, g_button_state_attr_handle, button_notify,
                 &snapshot.button_pressed, sizeof(snapshot.button_pressed));
    notify_value(conn, g_temperature_attr_handle, temperature_notify,
                 &snapshot.temperature_c, sizeof(snapshot.temperature_c));
    notify_value(conn, g_voltage_attr_handle, voltage_notify,
                 &snapshot.voltage_v, sizeof(snapshot.voltage_v));
    notify_value(conn, g_current_attr_handle, current_notify,
                 &snapshot.current_ma, sizeof(snapshot.current_ma));
}
