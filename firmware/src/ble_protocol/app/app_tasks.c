#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "event_bus.h"
#include "pair_mode.h"
#include "runtime_events.h"
#include "state.h"

static int64_t wall_time_us(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_usec;
}

void pair_timeout_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Timer pair_timeout expired active=%d",
             (int)pair_mode_is_active());
    if (!pair_mode_is_active()) {
        return;
    }

    uint32_t deadline_ms = pair_mode_deadline_ms();
    if (deadline_ms == 0 ||
        esp_timer_get_time() < (int64_t)deadline_ms * 1000LL) {
        /* A newer pairing window was opened after this timer was queued. */
        ESP_LOGI(TAG, "stale pair timeout ignored");
        return;
    }

    uint16_t conn_handle = fsm_get_conn_handle();
    runtime_event_t event = runtime_event_make(RTEVT_BLE_PAIR_TIMEOUT,
                                               wall_time_us());
    event.request_id = deadline_ms;
    event.conn_handle = conn_handle;
    /* An unauthenticated client must be disconnected when pairing mode ends.
     * The dispatcher deactivates pairing mode before the BLE FSM sees this. */
    event.bool_value = conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                       !fsm_is_authed();
    if (!event_bus_publish(&event)) {
        ESP_LOGW(TAG, "pair timeout event rejected");
    }
}
