#include "device_status.h"

#include <stdbool.h>
#include <string.h>

#include "device_status_ble.h"
#include "metrics.h"
#include "state.h"

typedef struct {
    device_state_t state;
    device_error_mask_t error_mask;
} device_status_cache_t;

static device_status_cache_t g_status = {
    .state = DEVICE_STATE_OK,
    .error_mask = DEVICE_ERROR_NONE,
};

static void device_status_notify_current(void) {
    uint8_t payload[DEVICE_STATUS_PAYLOAD_LEN];
    device_status_get_payload(payload, sizeof(payload));
    device_status_notify(fsm_get_conn_handle(), payload, sizeof(payload));
}

static void device_status_refresh_locked(bool *changed) {
    device_state_t state = (g_status.error_mask == DEVICE_ERROR_NONE) ? DEVICE_STATE_OK : DEVICE_STATE_ERROR;
    if (g_status.state != state) {
        g_status.state = state;
        *changed = true;
    }
}

void device_status_init(void) {
    state_lock();
    g_status.error_mask = metrics_has_error() ? DEVICE_ERROR_ADC_OFFLINE : DEVICE_ERROR_NONE;
    bool changed = false;
    device_status_refresh_locked(&changed);
    state_unlock();
    if (changed) {
        device_status_notify_current();
    }
}

void device_status_set_error_flag(device_error_mask_t flag, bool active) {
    if (flag == DEVICE_ERROR_NONE) return;
    bool changed = false;

    state_lock();
    device_error_mask_t next_mask = g_status.error_mask;
    if (active) {
        next_mask |= flag;
    } else {
        next_mask &= ~flag;
    }
    if (next_mask != g_status.error_mask) {
        g_status.error_mask = next_mask;
        changed = true;
        device_status_refresh_locked(&changed);
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

bool device_status_has_error_flag(device_error_mask_t flag) {
    bool active;
    if (flag == DEVICE_ERROR_NONE) return false;
    state_lock();
    active = (g_status.error_mask & flag) != 0;
    state_unlock();
    return active;
}

void device_status_get_payload(uint8_t *out, size_t len) {
    if (!out || len < DEVICE_STATUS_PAYLOAD_LEN) return;
    state_lock();
    out[0] = DEVICE_STATUS_VERSION;
    out[1] = (uint8_t)g_status.state;
    memcpy(out + 2, &g_status.error_mask, sizeof(g_status.error_mask));
    state_unlock();
}
