#include "load_metrics_ble.h"

#include "esp_log.h"
#include "os/os_mbuf.h"
#include "os/os_mempool.h"
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

/* Telemetry is expendable; leave space for incoming ATT requests/responses.
 * Check the actual pool chosen by the allocator, not the sum across pools:
 * free blocks in another pool do not necessarily satisfy a small allocation.
 */
#define METRICS_MBUF_RESERVE 4U

static bool notify_value(uint16_t conn, uint16_t handle, bool enabled,
                         const void *value, uint16_t length) {
    if (!enabled || handle == 0) return true;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, length);
    if (!om) {
        ESP_LOGW("ble-metrics", "Telemetry skipped: no mbuf free=%d", os_msys_num_free());
        return false;
    }
    unsigned pool_free = om->om_omp->omp_pool->mp_num_free;
    if (pool_free < METRICS_MBUF_RESERVE) {
        os_mbuf_free_chain(om);
        ESP_LOGD("ble-metrics", "Telemetry deferred: pool_free=%u reserve=%u",
                 pool_free, (unsigned)METRICS_MBUF_RESERVE);
        return false;
    }
    /* NimBLE consumes om on both success and failure. Do not free it again. */
    int rc = ble_gatts_notify_custom(conn, handle, om);
    if (rc != 0) {
        ESP_LOGW("ble-metrics", "Telemetry notify failed handle=%u rc=%d free=%d",
                 (unsigned)handle, rc, os_msys_num_free());
    }
    return rc == 0;
}

void load_metrics_publish(void) {
    uint16_t conn = fsm_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE || !fsm_is_authed() || !can_access_data()) return;

    load_runtime_snapshot_t snapshot;
    load_runtime_get_snapshot(&snapshot);
    if (!notify_value(conn, g_load_state_attr_handle, state_notify,
                 &snapshot.load_state, sizeof(snapshot.load_state))) return;
    if (!notify_value(conn, g_button_state_attr_handle, button_notify,
                 &snapshot.button_pressed, sizeof(snapshot.button_pressed))) return;
    if (!notify_value(conn, g_temperature_attr_handle, temperature_notify,
                 &snapshot.temperature_c, sizeof(snapshot.temperature_c))) return;
    if (!notify_value(conn, g_voltage_attr_handle, voltage_notify,
                 &snapshot.voltage_v, sizeof(snapshot.voltage_v))) return;
    (void)notify_value(conn, g_current_attr_handle, current_notify,
                 &snapshot.current_ma, sizeof(snapshot.current_ma));
}
