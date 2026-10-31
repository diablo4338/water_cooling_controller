#ifndef LOAD_FSM_H
#define LOAD_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime_commands.h"
#include "runtime_events.h"

typedef enum {
    LOAD_FSM_IDLE = 0,
    LOAD_FSM_STARTING,
    LOAD_FSM_WORKING,
    LOAD_FSM_STOPPING,
} load_fsm_state_t;

typedef struct {
    float low_current_ma;
    int64_t low_current_timeout_us;
} load_fsm_config_t;

typedef struct {
    load_fsm_state_t state;
    bool output_on;             /* confirmed by the GPIO executor */
    bool off_requested;
    bool low_current_pending;
    int64_t low_current_since_us;
    uint32_t operation_id;      /* current startup or shutdown operation */
    uint32_t startup_operation_id;
    uint32_t shutdown_operation_id;
    uint8_t off_retries;
} load_fsm_t;

void load_fsm_init(load_fsm_t *fsm, bool restored_working,
                   bool low_current_pending, int64_t low_current_since_us);

void load_fsm_step(load_fsm_t *fsm, const runtime_event_t *event,
                   const load_fsm_config_t *config,
                   runtime_command_list_t *commands);

const char *load_fsm_state_name(load_fsm_state_t state);

#endif
