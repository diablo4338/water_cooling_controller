#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>

#include "ble_fsm.h"
#include "buzzer_fsm.h"
#include "load_fsm.h"

typedef enum {
    POWER_SLEEP_ALLOWED = 0,
    POWER_BLOCK_BLE,
    POWER_BLOCK_BLE_CONNECTED,
    POWER_BLOCK_BLE_RADIO,
    POWER_BLOCK_LOAD_TRANSITION,
    POWER_BLOCK_LOAD_ERROR,
    POWER_BLOCK_BUZZER,
    POWER_BLOCK_ACTION,
    POWER_BLOCK_EVENT,
    POWER_BLOCK_CRITICAL,
} power_sleep_block_reason_t;

typedef struct {
    ble_fsm_state_t ble_state;
    load_fsm_state_t load_state;
    buzzer_fsm_state_t buzzer_state;
    bool ble_connected;
    bool ble_radio_quiet;
    bool action_pending;
    bool event_pending;
    bool critical_pending;
} power_snapshot_t;

power_sleep_block_reason_t power_manager_sleep_block_reason(
    const power_snapshot_t *snapshot);
bool power_manager_can_sleep(const power_snapshot_t *snapshot);
const char *power_manager_block_reason_name(power_sleep_block_reason_t reason);

#endif
