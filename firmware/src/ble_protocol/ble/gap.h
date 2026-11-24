#ifndef GAP_H
#define GAP_H

#include <stdbool.h>
#include <stdint.h>
#include "runtime_events.h"

/* Reset GAP state before lazy NimBLE initialization. */
void gap_init(void);
void gap_publish_metrics(void);
bool gap_take_pending_disconnect(runtime_event_t *out);

/* Initialize the controller lazily; recreate the host for each BLE window. */
bool gap_ble_start(uint32_t operation_id);
/* Stop the host and disable the controller before acknowledging the runtime. */
bool gap_ble_stop(void);
bool gap_is_initialized(void);
bool gap_is_radio_quiet(void);

bool gap_start_advertising(uint32_t operation_id, uint32_t request_id,
                           uint32_t duration_ms);
bool gap_refresh_advertising(uint32_t operation_id, uint32_t request_id,
                             uint32_t duration_ms);
void gap_disconnect_client(uint16_t conn_handle);

void ble_app_on_sync(void);
void ble_host_task(void *param);
void term_cb(void *arg);

#endif
