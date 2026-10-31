#include "load_fsm.h"

#include <math.h>

#define LOAD_OFF_MAX_RETRIES 3U

const char *load_fsm_state_name(load_fsm_state_t state) {
    switch (state) {
        case LOAD_FSM_IDLE: return "IDLE";
        case LOAD_FSM_STARTING: return "STARTING";
        case LOAD_FSM_WORKING: return "WORKING";
        case LOAD_FSM_STOPPING: return "STOPPING";
        case LOAD_FSM_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void load_fsm_init(load_fsm_t *fsm, bool restored_working,
                   bool low_current_pending, int64_t low_current_since_us) {
    if (!fsm) return;
    *fsm = (load_fsm_t) {
        .state = restored_working ? LOAD_FSM_WORKING : LOAD_FSM_IDLE,
        .output_on = restored_working,
        .low_current_pending = restored_working && low_current_pending,
        .low_current_since_us = low_current_since_us,
    };
}

static void command_output(load_fsm_t *fsm, runtime_command_list_t *commands,
                           runtime_command_kind_t kind) {
    runtime_command_t command = runtime_command_make(kind, fsm->operation_id);
    runtime_command_list_push(commands, &command);
}

static void begin_startup(load_fsm_t *fsm, runtime_command_list_t *commands) {
    fsm->operation_id = runtime_event_next_id();
    fsm->startup_operation_id = fsm->operation_id;
    fsm->state = LOAD_FSM_STARTING;
    fsm->output_on = false;
    fsm->off_requested = false;
    fsm->off_retries = 0;
    fsm->low_current_pending = false;
    command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
}

static void begin_shutdown(load_fsm_t *fsm, runtime_command_list_t *commands) {
    fsm->operation_id = runtime_event_next_id();
    fsm->shutdown_operation_id = fsm->operation_id;
    fsm->state = LOAD_FSM_STOPPING;
    fsm->low_current_pending = false;
    fsm->off_requested = false;
    fsm->off_retries = 0;
    /* The orchestrator turns this command into a Buzzer shutdown event; the
     * Load FSM never calls the Buzzer FSM directly. */
    runtime_command_t request = runtime_command_make(
        RCMD_BUZZER_REQUEST_SHUTDOWN, fsm->operation_id);
    runtime_command_list_push(commands, &request);
}

static void fail_load_off(load_fsm_t *fsm, runtime_command_list_t *commands) {
    if (fsm->off_retries < LOAD_OFF_MAX_RETRIES) {
        ++fsm->off_retries;
        command_output(fsm, commands, RCMD_LOAD_OUTPUT_OFF);
    } else {
        fsm->state = LOAD_FSM_ERROR;
        fsm->output_on = true;
    }
}

void load_fsm_step(load_fsm_t *fsm, const runtime_event_t *event,
                   const load_fsm_config_t *config,
                   runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    switch (event->kind) {
        case RTEVT_EVENT_BUS_FAULT:
            if (fsm->state == LOAD_FSM_STARTING ||
                fsm->state == LOAD_FSM_WORKING ||
                fsm->state == LOAD_FSM_STOPPING) {
                fsm->output_on = true;
            }
            fsm->state = LOAD_FSM_ERROR;
            fsm->low_current_pending = false;
            return;

        case RTEVT_THERMAL_BUTTON_PRESSED:
            if (fsm->state == LOAD_FSM_IDLE) {
                begin_startup(fsm, commands);
            }
            /* WORKING ignores presses; STARTING/STOPPING reject duplicates. */
            return;

        case RTEVT_LOAD_GPIO_APPLIED:
            if (event->operation_id != fsm->operation_id) return;
            if (fsm->state == LOAD_FSM_STARTING) {
                if (event->bool_value) {
                    fsm->output_on = true;
                } else {
                    fsm->output_on = false;
                    fsm->state = LOAD_FSM_IDLE;
                    fsm->low_current_pending = false;
                }
            } else if (fsm->state == LOAD_FSM_STOPPING) {
                if (!event->bool_value) {
                    fsm->output_on = false;
                    fsm->state = LOAD_FSM_IDLE;
                    fsm->off_requested = false;
                    fsm->off_retries = 0;
                    fsm->low_current_pending = false;
                }
            }
            return;

        case RTEVT_LOAD_GPIO_FAILED:
            if (event->operation_id != fsm->operation_id) return;
            if (fsm->state == LOAD_FSM_STARTING) {
                fsm->output_on = false;
                fsm->state = LOAD_FSM_IDLE;
                fsm->low_current_pending = false;
            } else if (fsm->state == LOAD_FSM_STOPPING && fsm->off_requested) {
                fail_load_off(fsm, commands);
            }
            return;

        case RTEVT_ACTION_FAILED:
            if (event->operation_id != fsm->operation_id) return;
            if (event->command_kind == RCMD_LOAD_OUTPUT_ON &&
                fsm->state == LOAD_FSM_STARTING) {
                fsm->output_on = false;
                fsm->state = LOAD_FSM_IDLE;
                fsm->low_current_pending = false;
            } else if (event->command_kind == RCMD_LOAD_OUTPUT_OFF &&
                       fsm->state == LOAD_FSM_STOPPING &&
                       fsm->off_requested) {
                fail_load_off(fsm, commands);
            }
            return;

        case RTEVT_BUZZER_DONE:
            if (fsm->state == LOAD_FSM_STARTING &&
                event->buzzer_operation == BUZZER_OP_STARTUP &&
                event->operation_id == fsm->startup_operation_id) {
                fsm->state = event->error == 0 ? LOAD_FSM_WORKING
                                               : LOAD_FSM_ERROR;
                fsm->operation_id = fsm->startup_operation_id;
                fsm->output_on = true;
                fsm->low_current_pending = false;
            } else if (fsm->state == LOAD_FSM_STOPPING &&
                       event->buzzer_operation == BUZZER_OP_SHUTDOWN &&
                       event->operation_id == fsm->shutdown_operation_id &&
                       !fsm->off_requested) {
                fsm->off_requested = true;
                command_output(fsm, commands, RCMD_LOAD_OUTPUT_OFF);
            }
            return;

        case RTEVT_INA_SAMPLE_OK:
            if (fsm->state != LOAD_FSM_WORKING ||
                event->context != MEASURE_PERIODIC) {
                return;
            }
            if (!isfinite(event->current_ma) ||
                fabsf(event->current_ma) >= config->low_current_ma) {
                fsm->low_current_pending = false;
                return;
            }
            if (!fsm->low_current_pending ||
                event->timestamp_us < fsm->low_current_since_us) {
                fsm->low_current_pending = true;
                fsm->low_current_since_us = event->timestamp_us;
            } else if (event->timestamp_us - fsm->low_current_since_us >=
                       config->low_current_timeout_us) {
                begin_shutdown(fsm, commands);
            }
            return;

        case RTEVT_INA_SAMPLE_FAILED:
            if (fsm->state == LOAD_FSM_WORKING &&
                event->context == MEASURE_PERIODIC) {
                /* Invalid current is unknown, never low.  Restart the interval. */
                fsm->low_current_pending = false;
            }
            return;

        default:
            return;
    }
}
