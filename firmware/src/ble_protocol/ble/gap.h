#ifndef GAP_H
#define GAP_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize non-blocking NimBLE event wrappers.  Does not start the radio. */
void gap_init(void);

/* Lazy lifecycle.  First call initializes the host/controller once per uptime;
 * subsequent calls re-enable the controller and restart the stopped host
 * without deinitializing it. */
bool gap_ble_start(uint32_t operation_id);
bool gap_ble_stop(uint32_t operation_id);
bool gap_is_initialized(void);
bool gap_is_radio_quiet(void);

bool gap_start_advertising(uint32_t operation_id, uint32_t request_id,
                           uint32_t duration_ms);
bool gap_refresh_advertising(uint32_t operation_id, uint32_t request_id,
                             uint32_t duration_ms);
void gap_stop_advertising(void);
void gap_disconnect_client(void);

void ble_app_on_sync(void);
void ble_host_task(void *param);
void term_cb(void *arg);

#endif
