#ifndef BLE_FSM_H
#define BLE_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime_commands.h"
#include "runtime_events.h"

typedef enum {
    BLE_FSM_OFF = 0,
    BLE_FSM_STARTING,
    BLE_FSM_ADVERTISING,
    BLE_FSM_CONNECTED,
    BLE_FSM_READY_TO_SLEEP,
} ble_fsm_state_t;

typedef struct {
    int64_t window_us;
} ble_fsm_config_t;

typedef struct {
    ble_fsm_state_t state;
    int64_t window_deadline_us;
    int64_t startup_deadline_us;
    int64_t connection_deadline_us;
    uint32_t operation_id;       /* current start generation */
    uint32_t adv_request_id;     /* current advertising generation */
    uint16_t conn_handle;
} ble_fsm_t;

void ble_fsm_init(ble_fsm_t *fsm, int64_t now_us);
void ble_fsm_step(ble_fsm_t *fsm, const runtime_event_t *event,
                  const ble_fsm_config_t *config, int64_t now_us,
                  runtime_command_list_t *commands);

int32_t ble_fsm_remaining_ms(const ble_fsm_t *fsm, int64_t now_us);
bool ble_fsm_is_active(const ble_fsm_t *fsm);
bool ble_fsm_blocks_sleep(const ble_fsm_t *fsm);
const char *ble_fsm_state_name(ble_fsm_state_t state);

#endif
