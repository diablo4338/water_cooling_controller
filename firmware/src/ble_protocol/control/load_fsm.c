#include "load_fsm.h"

#include <math.h>

#define LOAD_OFF_MAX_RETRIES 3U
#define LOAD_ON_MAX_RETRIES 3U
#define LOAD_VERIFY_MAX_RETRIES 3U
#define LOAD_ACTION_ACK_TIMEOUT_US 1000000LL

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
    command.bool_value = kind == RCMD_LOAD_OUTPUT_ON;
    runtime_command_list_push(commands, &command);
}

static void begin_startup(load_fsm_t *fsm, int64_t now_us,
                          runtime_command_list_t *commands) {
    fsm->operation_id = runtime_event_next_id();
    fsm->startup_operation_id = fsm->operation_id;
    fsm->state = LOAD_FSM_STARTING;
    fsm->output_on = false;
    fsm->off_requested = false;
    fsm->off_retries = 0;
    fsm->on_retries = 0;
    fsm->verification_retries = 0;
    fsm->verification_pending = false;
    fsm->low_current_pending = false;
    fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
    fsm->feedback_deadline_us = 0;
    command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
}

static void begin_shutdown(load_fsm_t *fsm, runtime_command_list_t *commands) {
    fsm->operation_id = runtime_event_next_id();
    fsm->shutdown_operation_id = fsm->operation_id;
    fsm->state = LOAD_FSM_STOPPING;
    fsm->low_current_pending = false;
    fsm->off_requested = false;
    fsm->off_retries = 0;
    fsm->verification_retries = 0;
    fsm->verification_pending = false;
    fsm->action_deadline_us = 0;
    fsm->feedback_deadline_us = 0;
    /* The orchestrator turns this command into a Buzzer shutdown event; the
     * Load FSM never calls the Buzzer FSM directly. */
    runtime_command_t request = runtime_command_make(
        RCMD_BUZZER_REQUEST_SHUTDOWN, fsm->operation_id);
    runtime_command_list_push(commands, &request);
}

static void fail_load_off(load_fsm_t *fsm, int64_t now_us,
                          runtime_command_list_t *commands) {
    fsm->verification_pending = false;
    fsm->verification_retries = 0;
    if (fsm->off_retries < LOAD_OFF_MAX_RETRIES) {
        ++fsm->off_retries;
        fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
        command_output(fsm, commands, RCMD_LOAD_OUTPUT_OFF);
    } else {
        fsm->state = LOAD_FSM_ERROR;
        fsm->output_on = true;
        fsm->action_deadline_us = 0;
    }
}

static void request_gpio_verification(load_fsm_t *fsm, bool expected_on,
                                      int64_t now_us,
                                      runtime_command_list_t *commands) {
    runtime_command_t command = runtime_command_make(
        RCMD_LOAD_OUTPUT_VERIFY, fsm->operation_id);
    command.bool_value = expected_on;
    runtime_command_list_push(commands, &command);
    fsm->verification_pending = true;
    fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
}

static void retry_load_on(load_fsm_t *fsm, int64_t now_us,
                          runtime_command_list_t *commands) {
    fsm->verification_pending = false;
    fsm->verification_retries = 0;
    if (fsm->on_retries < LOAD_ON_MAX_RETRIES) {
        ++fsm->on_retries;
        fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
        command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
    } else {
        fsm->state = LOAD_FSM_ERROR;
        fsm->output_on = false;
        fsm->action_deadline_us = 0;
    }
}

static bool is_current_gpio_verification(const load_fsm_t *fsm,
                                         const runtime_event_t *event) {
    if (!fsm->verification_pending ||
        event->operation_id != fsm->operation_id) {
        return false;
    }
    if (fsm->state == LOAD_FSM_STARTING) {
        return event->command_kind == RCMD_LOAD_OUTPUT_ON;
    }
    if (fsm->state == LOAD_FSM_STOPPING && fsm->off_requested) {
        return event->command_kind == RCMD_LOAD_OUTPUT_OFF;
    }
    return false;
}

static void handle_gpio_verification(load_fsm_t *fsm,
                                     const runtime_event_t *event,
                                     int64_t now_us,
                                     runtime_command_list_t *commands) {
    if (!is_current_gpio_verification(fsm, event)) return;
    fsm->verification_pending = false;
    fsm->action_deadline_us = 0;

    if (event->error == 0 && event->bool_value) {
        fsm->verification_retries = 0;
        if (fsm->state == LOAD_FSM_STARTING) {
            fsm->output_on = true;
        } else {
            fsm->output_on = false;
            fsm->state = LOAD_FSM_IDLE;
            fsm->off_requested = false;
            fsm->off_retries = 0;
            fsm->low_current_pending = false;
        }
        return;
    }

    if (fsm->state == LOAD_FSM_STARTING) {
        retry_load_on(fsm, now_us, commands);
    } else {
        fail_load_off(fsm, now_us, commands);
    }
}

void load_fsm_step(load_fsm_t *fsm, const runtime_event_t *event,
                   const load_fsm_config_t *config,
                   int64_t now_us,
                   runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    switch (event->kind) {
        case RTEVT_EVENT_BUS_FAULT:
            /* Per-operation deadlines, not delivery diagnostics, resolve work. */
            return;

        case RTEVT_THERMAL_BUTTON_PRESSED:
            if (fsm->state == LOAD_FSM_IDLE) {
                begin_startup(fsm, now_us, commands);
            }
            /* WORKING ignores presses; STARTING/STOPPING reject duplicates. */
            return;

        case RTEVT_LOAD_GPIO_APPLIED:
            if (event->operation_id != fsm->operation_id) return;
            if (fsm->state == LOAD_FSM_STARTING) {
                if (!event->bool_value || fsm->output_on) return;
                fsm->output_on = true;
                fsm->verification_pending = false;
                fsm->verification_retries = 0;
                fsm->action_deadline_us = 0;
            } else if (fsm->state == LOAD_FSM_STOPPING) {
                if (event->bool_value || !fsm->off_requested) return;
                fsm->output_on = false;
                fsm->state = LOAD_FSM_IDLE;
                fsm->verification_pending = false;
                fsm->verification_retries = 0;
                fsm->action_deadline_us = 0;
                fsm->feedback_deadline_us = 0;
                fsm->off_requested = false;
                fsm->off_retries = 0;
                fsm->low_current_pending = false;
            }
            return;

        case RTEVT_LOAD_GPIO_FAILED:
            if (event->operation_id != fsm->operation_id) return;
            if (fsm->state == LOAD_FSM_STARTING &&
                event->bool_value && !fsm->output_on) {
                fsm->output_on = false;
                fsm->state = LOAD_FSM_ERROR;
                fsm->verification_pending = false;
                fsm->action_deadline_us = 0;
                fsm->low_current_pending = false;
            } else if (fsm->state == LOAD_FSM_STOPPING &&
                       !event->bool_value && fsm->off_requested) {
                fail_load_off(fsm, now_us, commands);
            }
            return;

        case RTEVT_LOAD_GPIO_VERIFIED:
            handle_gpio_verification(fsm, event, now_us, commands);
            return;

        case RTEVT_ACTION_FAILED:
            if (event->operation_id != fsm->operation_id) return;
            if (event->command_kind == RCMD_LOAD_OUTPUT_VERIFY) {
                runtime_event_t failed_check = *event;
                failed_check.kind = RTEVT_LOAD_GPIO_VERIFIED;
                failed_check.command_kind = event->bool_value
                                                ? RCMD_LOAD_OUTPUT_ON
                                                : RCMD_LOAD_OUTPUT_OFF;
                failed_check.bool_value = false;
                handle_gpio_verification(fsm, &failed_check, now_us, commands);
                return;
            }
            if (event->command_kind == RCMD_LOAD_OUTPUT_ON &&
                fsm->state == LOAD_FSM_STARTING && !fsm->output_on) {
                if (fsm->on_retries == 0) {
                    fsm->output_on = false;
                    fsm->state = LOAD_FSM_IDLE;
                    fsm->verification_pending = false;
                    fsm->action_deadline_us = 0;
                    fsm->low_current_pending = false;
                }
            } else if (event->command_kind == RCMD_LOAD_OUTPUT_OFF &&
                       fsm->state == LOAD_FSM_STOPPING &&
                       fsm->off_requested) {
                fail_load_off(fsm, now_us, commands);
            }
            return;

        case RTEVT_BUZZER_DONE:
            if (fsm->state == LOAD_FSM_STARTING &&
                event->buzzer_operation == BUZZER_OP_STARTUP &&
                event->operation_id == fsm->startup_operation_id) {
                fsm->state = LOAD_FSM_WORKING;
                fsm->operation_id = fsm->startup_operation_id;
                fsm->output_on = true;
                fsm->feedback_deadline_us = 0;
                fsm->low_current_pending = false;
            } else if (fsm->state == LOAD_FSM_STOPPING &&
                       event->buzzer_operation == BUZZER_OP_SHUTDOWN &&
                       event->operation_id == fsm->shutdown_operation_id &&
                       !fsm->off_requested) {
                fsm->feedback_deadline_us = 0;
                fsm->off_requested = true;
                fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
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

        case RTEVT_DEADLINE_CHECK:
            if (fsm->feedback_deadline_us != 0 &&
                now_us >= fsm->feedback_deadline_us) {
                fsm->feedback_deadline_us = 0;
                if (fsm->state == LOAD_FSM_STARTING) {
                    runtime_command_t cancel = runtime_command_make(
                        RCMD_BUZZER_CANCEL, fsm->startup_operation_id);
                    runtime_command_list_push(commands, &cancel);
                    fsm->state = LOAD_FSM_WORKING;
                    fsm->operation_id = fsm->startup_operation_id;
                    fsm->output_on = true;
                    fsm->low_current_pending = false;
                } else if (fsm->state == LOAD_FSM_STOPPING &&
                           !fsm->off_requested) {
                    runtime_command_t cancel = runtime_command_make(
                        RCMD_BUZZER_CANCEL, fsm->shutdown_operation_id);
                    runtime_command_list_push(commands, &cancel);
                    fsm->off_requested = true;
                    fsm->action_deadline_us =
                        now_us + LOAD_ACTION_ACK_TIMEOUT_US;
                    command_output(fsm, commands, RCMD_LOAD_OUTPUT_OFF);
                }
                return;
            }
            if (fsm->action_deadline_us == 0 ||
                now_us < fsm->action_deadline_us) {
                return;
            }
            if (fsm->state == LOAD_FSM_STARTING) {
                if (fsm->verification_pending) {
                    if (fsm->verification_retries < LOAD_VERIFY_MAX_RETRIES) {
                        ++fsm->verification_retries;
                        request_gpio_verification(fsm, true, now_us, commands);
                    } else {
                        retry_load_on(fsm, now_us, commands);
                    }
                } else {
                    request_gpio_verification(fsm, true, now_us, commands);
                }
            } else if (fsm->state == LOAD_FSM_STOPPING &&
                       fsm->off_requested) {
                if (fsm->verification_pending) {
                    if (fsm->verification_retries < LOAD_VERIFY_MAX_RETRIES) {
                        ++fsm->verification_retries;
                        request_gpio_verification(fsm, false, now_us, commands);
                    } else {
                        fail_load_off(fsm, now_us, commands);
                    }
                } else {
                    request_gpio_verification(fsm, false, now_us, commands);
                }
            }
            return;

        default:
            return;
    }
}
