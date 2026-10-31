#ifndef RUNTIME_CORE_H
#define RUNTIME_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "ble_fsm.h"
#include "buzzer_fsm.h"
#include "load_fsm.h"
#include "runtime_commands.h"
#include "runtime_events.h"

/*
 * Pure coordination layer for the three runtime FSMs.  It owns no hardware
 * and performs no side effects: a single event is offered to Load, Buzzer and
 * BLE in a deterministic order and the resulting commands are returned to the
 * caller.
 */
typedef struct {
    load_fsm_t load;
    buzzer_fsm_t buzzer;
    ble_fsm_t ble;

    load_fsm_config_t load_config;
    buzzer_fsm_config_t buzzer_config;
    ble_fsm_config_t ble_config;
} runtime_core_t;

void runtime_core_init(runtime_core_t *core,
                       const load_fsm_config_t *load_config,
                       const buzzer_fsm_config_t *buzzer_config,
                       const ble_fsm_config_t *ble_config,
                       bool restored_working,
                       bool load_low_pending,
                       int64_t load_low_since_us,
                       bool buzzer_low_pending,
                       bool buzzer_low_alerted,
                       int64_t buzzer_low_since_us);

void runtime_core_step(runtime_core_t *core, const runtime_event_t *event,
                       int64_t now_us, runtime_command_list_t *commands);

/*
 * Convert a dispatcher-level command into the event that must be fed back to
 * the FSMs.  Returns false for commands that are hardware actions.
 */
bool runtime_core_command_event(const runtime_command_t *command,
                                runtime_event_t *event);

#endif
