#include "device_status.h"

#include <stdbool.h>

#include "device_status_ble.h"
#include "metrics.h"
#include "state.h"

typedef struct {
    device_state_t state;
    device_error_t error;
} device_status_cache_t;

static device_status_cache_t g_status = {
    .state = DEVICE_STATE_OK,
    .error = DEVICE_ERROR_NONE,
};

static void device_status_notify_current(void) {
    uint8_t payload[DEVICE_STATUS_PAYLOAD_LEN];
    device_status_get_payload(payload, sizeof(payload));
    device_status_notify(fsm_get_conn_handle(), payload, sizeof(payload));
}

void device_status_init(void) {
    device_status_set_error(metrics_has_error() ? DEVICE_ERROR_ADC_OFFLINE : DEVICE_ERROR_NONE);
}

void device_status_set_error(device_error_t error) {
    device_state_t state = (error == DEVICE_ERROR_NONE) ? DEVICE_STATE_OK : DEVICE_STATE_ERROR;
    bool changed = false;

    state_lock();
    if (g_status.state != state || g_status.error != error) {
        g_status.state = state;
        g_status.error = error;
        changed = true;
    }
    state_unlock();

    if (changed) {
        device_status_notify_current();
    }
}

bool device_status_is_error(void) {
    bool is_error;
    state_lock();
    is_error = g_status.state != DEVICE_STATE_OK;
    state_unlock();
    return is_error;
}

void device_status_get_payload(uint8_t *out, size_t len) {
    if (!out || len < DEVICE_STATUS_PAYLOAD_LEN) return;
    state_lock();
    out[0] = DEVICE_STATUS_VERSION;
    out[1] = (uint8_t)g_status.state;
    out[2] = (uint8_t)g_status.error;
    state_unlock();
}
