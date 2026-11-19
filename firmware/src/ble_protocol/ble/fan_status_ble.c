#include "fan_status_ble.h"

#include <string.h>
#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "access.h"
#include "fan_control.h"
#include "output_limit.h"
#include "params.h"
#include "state.h"

uint16_t g_fan_status_attr_handle = 0;
static bool g_fan_status_notify_enabled = false;
uint16_t g_fan_mode_status_attr_handle;
uint16_t g_output_limit_attr_handle;
uint16_t g_output_range_attr_handle;

typedef struct {
    uint16_t *handle;
    bool enabled;
    uint32_t subscription_revision;
    uint32_t sent_revision;
    uint8_t sent[OUTPUT_LIMIT_EXTENDED_LEN];
} runtime_subscription_t;

static runtime_subscription_t g_runtime_subscriptions[] = {
    {.handle = &g_fan_mode_status_attr_handle},
    {.handle = &g_output_limit_attr_handle},
    {.handle = &g_output_range_attr_handle},
};

void fan_status_ble_init(void) {
    g_fan_status_attr_handle = 0;
    g_fan_status_notify_enabled = false;
}

void fan_status_set_notify(uint16_t attr_handle, bool enabled) {
    if (attr_handle == 0) return;
    if (attr_handle == g_fan_status_attr_handle) g_fan_status_notify_enabled = enabled;
    state_lock();
    for (size_t i = 0; i < sizeof(g_runtime_subscriptions) / sizeof(g_runtime_subscriptions[0]); i++) {
        runtime_subscription_t *subscription = &g_runtime_subscriptions[i];
        if (attr_handle == *subscription->handle && subscription->enabled != enabled) {
            subscription->enabled = enabled;
            subscription->subscription_revision++;
        }
    }
    state_unlock();
}

void fan_status_reset_notify(void) {
    g_fan_status_notify_enabled = false;
    state_lock();
    for (size_t i = 0; i < sizeof(g_runtime_subscriptions) / sizeof(g_runtime_subscriptions[0]); i++) {
        g_runtime_subscriptions[i].enabled = false;
        g_runtime_subscriptions[i].subscription_revision++;
    }
    state_unlock();
}

void fan_status_notify(uint16_t conn_handle, const uint8_t *payload, size_t len) {
    if (!g_fan_status_notify_enabled) return;
    if (g_fan_status_attr_handle == 0) return;
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!can_access_data(conn_handle)) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, g_fan_status_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify fan status rc=%d", rc);
    }
}

size_t output_limit_status_encode(bool extended, uint8_t *payload) {
    output_limit_status_t limit = output_limit_get_status();
    uint8_t maximum = params_get_output_max();
    uint8_t effective = extended ? limit.confirmed_effective : limit.effective;
    if (effective > maximum) effective = maximum;
    payload[0] = extended ? OUTPUT_LIMIT_EXTENDED_VERSION : OUTPUT_LIMIT_STATUS_VERSION;
    payload[1] = maximum;
    payload[2] = effective;
    output_limit_state_t state = limit.state;
    if (state == OUTPUT_LIMIT_WAIT_CONFIG) state = OUTPUT_LIMIT_MEASURE;
    if (state == OUTPUT_LIMIT_SEARCH_UP) state = OUTPUT_LIMIT_VERIFY_STEP;
    payload[3] = (uint8_t)state;
    payload[4] = limit.reset_sequence;
    payload[5] = (uint8_t)limit.reset_result;
    if (!extended) return OUTPUT_LIMIT_PAYLOAD_LEN;

    uint8_t minimum = limit.temporary_minimum;
    if (minimum != OUTPUT_LIMIT_NO_TEMPORARY_MINIMUM && minimum > effective) minimum = effective;
    payload[6] = minimum;
    payload[7] = limit.alert_without_load;
    return OUTPUT_LIMIT_EXTENDED_LEN;
}

void fan_runtime_status_publish(void) {
    uint16_t conn_handle = fsm_get_auth_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !can_access_data(conn_handle)) return;

    for (size_t i = 0; i < sizeof(g_runtime_subscriptions) / sizeof(g_runtime_subscriptions[0]); i++) {
        runtime_subscription_t *subscription = &g_runtime_subscriptions[i];
        state_lock();
        bool enabled = subscription->enabled;
        uint32_t revision = subscription->subscription_revision;
        state_unlock();
        if (!enabled) continue;

        uint8_t payload[OUTPUT_LIMIT_EXTENDED_LEN];
        size_t len;
        if (subscription->handle == &g_fan_mode_status_attr_handle) {
            len = FAN_MODE_STATUS_PAYLOAD_LEN;
            fan_control_get_mode_payload(payload, len);
        } else {
            len = output_limit_status_encode(subscription->handle == &g_output_range_attr_handle, payload);
        }
        if (subscription->sent_revision == revision && memcmp(subscription->sent, payload, len) == 0) continue;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, len);
        if (!om) continue;
        int rc = ble_gatts_notify_custom(conn_handle, *subscription->handle, om);
        if (rc == 0) {
            memcpy(subscription->sent, payload, len);
            subscription->sent_revision = revision;
        } else {
            ESP_LOGW(TAG, "notify runtime status handle=%u rc=%d", (unsigned)*subscription->handle, rc);
        }
    }
}
