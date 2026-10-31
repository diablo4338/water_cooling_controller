#include "buzzer_fsm.h"

#include <math.h>

const char *buzzer_fsm_state_name(buzzer_fsm_state_t state) {
    switch (state) {
        case BUZZER_FSM_IDLE: return "IDLE";
        case BUZZER_FSM_WAIT_SETTLE: return "WAIT_SETTLE";
        case BUZZER_FSM_MEASURING: return "MEASURING";
        case BUZZER_FSM_BEEPING: return "BEEPING";
        default: return "UNKNOWN";
    }
}

void buzzer_fsm_init(buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config,
                     bool low_pending, bool low_alerted, int64_t low_since_us) {
    if (!fsm) return;
    *fsm = (buzzer_fsm_t) {
        .state = BUZZER_FSM_IDLE,
        .operation = BUZZER_OP_WARNING,
        .threshold_mv = config ? config->threshold_mv : 0,
        .low_pending = low_pending,
        .low_alerted = low_alerted,
        .low_since_us = low_since_us,
    };
}

bool buzzer_fsm_is_active(const buzzer_fsm_t *fsm) {
    return fsm && fsm->state != BUZZER_FSM_IDLE;
}

static void emit_done(runtime_command_list_t *commands, uint32_t operation_id,
                      buzzer_operation_t operation) {
    runtime_command_t command = runtime_command_make(
        RCMD_EMIT_BUZZER_DONE, operation_id);
    command.buzzer_operation = operation;
    runtime_command_list_push(commands, &command);
}

static void play_pattern(runtime_command_list_t *commands, uint32_t operation_id,
                         buzzer_pattern_t pattern, uint32_t on_ms,
                         uint16_t off_ms, uint8_t repeats) {
    runtime_command_t command = runtime_command_make(
        RCMD_BUZZER_PLAY, operation_id);
    command.pattern = pattern;
    command.duration_ms = on_ms;
    command.off_ms = off_ms;
    command.repeats = repeats;
    runtime_command_list_push(commands, &command);
}

static void reset_low_state(buzzer_fsm_t *fsm) {
    fsm->low_pending = false;
    fsm->low_alerted = false;
    fsm->low_since_us = 0;
}

static void begin_startup(buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config,
                          uint32_t operation_id,
                          runtime_command_list_t *commands) {
    fsm->pending_startup = false;
    fsm->pending_startup_id = 0;
    fsm->operation = BUZZER_OP_STARTUP;
    fsm->operation_id = operation_id;
    fsm->request_id = 0;
    fsm->pattern = BUZZER_PATTERN_NONE;
    reset_low_state(fsm);

    if (!config->enabled) {
        fsm->state = BUZZER_FSM_IDLE;
        fsm->last_startup_operation_id = operation_id;
        emit_done(commands, operation_id, BUZZER_OP_STARTUP);
        return;
    }

    fsm->state = BUZZER_FSM_WAIT_SETTLE;
    runtime_command_t command = runtime_command_make(
        RCMD_START_SETTLE_TIMER, operation_id);
    command.duration_ms = config->settle_ms;
    runtime_command_list_push(commands, &command);
}

static void request_startup_measurement(buzzer_fsm_t *fsm,
                                        const buzzer_fsm_config_t *config,
                                        runtime_command_list_t *commands) {
    fsm->state = BUZZER_FSM_MEASURING;
    fsm->request_id = runtime_event_next_id();

    /* Arm the timeout before the (blocking) INA request so a successful or
     * failed sample can cancel the timer deterministically. */
    runtime_command_t timeout = runtime_command_make(
        RCMD_START_MEASURE_TIMEOUT, fsm->operation_id);
    timeout.request_id = fsm->request_id;
    timeout.context = MEASURE_STARTUP;
    timeout.duration_ms = config->measure_timeout_ms;
    runtime_command_list_push(commands, &timeout);

    runtime_command_t request = runtime_command_make(
        RCMD_INA_REQUEST, fsm->operation_id);
    request.request_id = fsm->request_id;
    request.context = MEASURE_STARTUP;
    runtime_command_list_push(commands, &request);
}

static void begin_shutdown(buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config,
                           uint32_t operation_id,
                           runtime_command_list_t *commands) {
    fsm->pending_startup = false;
    fsm->pending_startup_id = 0;
    fsm->operation = BUZZER_OP_SHUTDOWN;
    fsm->operation_id = operation_id;
    fsm->pattern = BUZZER_PATTERN_NONE;
    reset_low_state(fsm);

    if (!config->enabled) {
        fsm->state = BUZZER_FSM_IDLE;
        fsm->last_shutdown_operation_id = operation_id;
        emit_done(commands, operation_id, BUZZER_OP_SHUTDOWN);
        return;
    }

    fsm->state = BUZZER_FSM_MEASURING;
    fsm->request_id = runtime_event_next_id();

    runtime_command_t timeout = runtime_command_make(
        RCMD_START_MEASURE_TIMEOUT, operation_id);
    timeout.request_id = fsm->request_id;
    timeout.context = MEASURE_SHUTDOWN;
    timeout.duration_ms = config->measure_timeout_ms;
    runtime_command_list_push(commands, &timeout);

    runtime_command_t request = runtime_command_make(
        RCMD_INA_REQUEST, operation_id);
    request.request_id = fsm->request_id;
    request.context = MEASURE_SHUTDOWN;
    runtime_command_list_push(commands, &request);
}

static void complete_startup(buzzer_fsm_t *fsm,
                             runtime_command_list_t *commands) {
    fsm->state = BUZZER_FSM_IDLE;
    fsm->last_startup_operation_id = fsm->operation_id;
    fsm->request_id = 0;
    reset_low_state(fsm);
    emit_done(commands, fsm->operation_id, BUZZER_OP_STARTUP);
    if (fsm->pending_startup) {
        /* A pending startup is picked up by the next event; this keeps command
         * generation bounded and avoids recursive FSM calls. */
    }
}

static void complete_shutdown(buzzer_fsm_t *fsm,
                              runtime_command_list_t *commands) {
    fsm->state = BUZZER_FSM_IDLE;
    fsm->last_shutdown_operation_id = fsm->operation_id;
    fsm->request_id = 0;
    emit_done(commands, fsm->operation_id, BUZZER_OP_SHUTDOWN);
}

static void startup_measurement_failed(buzzer_fsm_t *fsm,
                                       const buzzer_fsm_config_t *config,
                                       runtime_command_list_t *commands) {
    fsm->request_id = 0;
    if (!config->enabled) {
        complete_startup(fsm, commands);
        return;
    }
    fsm->state = BUZZER_FSM_BEEPING;
    fsm->operation = BUZZER_OP_STARTUP;
    fsm->pattern = BUZZER_PATTERN_ERROR;
    play_pattern(commands, fsm->operation_id, BUZZER_PATTERN_ERROR,
                 config->error_on_ms, config->error_off_ms,
                 config->error_repeats);
}

static void handle_periodic_sample(buzzer_fsm_t *fsm,
                                   const buzzer_fsm_config_t *config,
                                   const runtime_event_t *event,
                                   runtime_command_list_t *commands) {
    if (fsm->threshold_mv != config->threshold_mv) {
        fsm->threshold_mv = config->threshold_mv;
        reset_low_state(fsm);
    }
    if (!isfinite(event->voltage_v)) {
        /* Preserve the existing policy: an invalid periodic reading is not a
         * low-voltage assertion and restarts the warning interval. */
        reset_low_state(fsm);
        return;
    }

    bool low = config->threshold_mv > 0 &&
               event->voltage_v * 1000.0f < (float)config->threshold_mv;
    if (!low) {
        reset_low_state(fsm);
        return;
    }
    if (!fsm->low_pending || event->timestamp_us < fsm->low_since_us) {
        fsm->low_pending = true;
        fsm->low_alerted = false;
        fsm->low_since_us = event->timestamp_us;
        return;
    }
    if (fsm->low_alerted ||
        event->timestamp_us - fsm->low_since_us <=
            (int64_t)config->low_delay_s * 1000000LL) {
        return;
    }

    fsm->low_alerted = true;
    if (!config->enabled || config->low_ms == 0) return;
    fsm->state = BUZZER_FSM_BEEPING;
    fsm->operation = BUZZER_OP_WARNING;
    fsm->operation_id = runtime_event_next_id();
    fsm->pattern = BUZZER_PATTERN_WARNING;
    play_pattern(commands, fsm->operation_id, BUZZER_PATTERN_WARNING,
                 config->low_ms, 0, 1);
}

void buzzer_fsm_step(buzzer_fsm_t *fsm, const runtime_event_t *event,
                     const buzzer_fsm_config_t *config,
                     runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    switch (event->kind) {
        case RTEVT_LOAD_GPIO_APPLIED:
            if (!event->bool_value) return;
            if (event->operation_id == fsm->last_startup_operation_id) return;
            if (fsm->state == BUZZER_FSM_IDLE) {
                begin_startup(fsm, config, event->operation_id, commands);
            } else if (fsm->state == BUZZER_FSM_BEEPING &&
                       fsm->operation == BUZZER_OP_WARNING) {
                fsm->pending_startup = true;
                fsm->pending_startup_id = event->operation_id;
            }
            /* Duplicate ACKs while a startup is in progress are ignored. */
            return;

        case RTEVT_BUZZER_REQUEST_SHUTDOWN:
            if (event->operation_id == fsm->last_shutdown_operation_id) return;
            if (fsm->state == BUZZER_FSM_BEEPING &&
                fsm->operation == BUZZER_OP_WARNING) {
                runtime_command_t cancel = runtime_command_make(
                    RCMD_BUZZER_CANCEL, fsm->operation_id);
                runtime_command_list_push(commands, &cancel);
                begin_shutdown(fsm, config, event->operation_id, commands);
            } else if (fsm->state == BUZZER_FSM_IDLE) {
                begin_shutdown(fsm, config, event->operation_id, commands);
            }
            return;

        case RTEVT_SETTLE_ELAPSED:
            if (fsm->state != BUZZER_FSM_WAIT_SETTLE ||
                event->operation_id != fsm->operation_id) {
                return;
            }
            request_startup_measurement(fsm, config, commands);
            return;

        case RTEVT_INA_SAMPLE_OK:
            if (fsm->state == BUZZER_FSM_MEASURING &&
                event->request_id == fsm->request_id &&
                event->operation_id == fsm->operation_id &&
                event->context == (fsm->operation == BUZZER_OP_STARTUP
                                       ? MEASURE_STARTUP
                                       : MEASURE_SHUTDOWN)) {
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    if (!isfinite(event->voltage_v)) {
                        startup_measurement_failed(fsm, config, commands);
                        return;
                    }
                    bool low = config->threshold_mv > 0 &&
                               event->voltage_v * 1000.0f <
                                   (float)config->threshold_mv;
                    uint32_t duration = low ? config->combined_ms
                                            : config->button_ms;
                    fsm->state = BUZZER_FSM_BEEPING;
                    fsm->pattern = low ? BUZZER_PATTERN_LOW_BATTERY
                                       : BUZZER_PATTERN_NORMAL;
                    play_pattern(commands, fsm->operation_id, fsm->pattern,
                                 duration, 0, 1);
                } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                    /* No shutdown tone is defined by the approved contract. */
                    complete_shutdown(fsm, commands);
                }
            } else if (fsm->state == BUZZER_FSM_IDLE &&
                       event->context == MEASURE_PERIODIC) {
                handle_periodic_sample(fsm, config, event, commands);
            }
            return;

        case RTEVT_INA_SAMPLE_FAILED:
            if (fsm->state == BUZZER_FSM_MEASURING &&
                event->request_id == fsm->request_id &&
                event->operation_id == fsm->operation_id &&
                event->context == (fsm->operation == BUZZER_OP_STARTUP
                                       ? MEASURE_STARTUP
                                       : MEASURE_SHUTDOWN)) {
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    startup_measurement_failed(fsm, config, commands);
                } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                    complete_shutdown(fsm, commands);
                }
            } else if (fsm->state == BUZZER_FSM_IDLE &&
                       event->context == MEASURE_PERIODIC) {
                /* Periodic failures are silent. */
                reset_low_state(fsm);
            }
            return;

        case RTEVT_MEASURE_TIMEOUT:
            if (fsm->state == BUZZER_FSM_MEASURING &&
                event->request_id == fsm->request_id &&
                event->operation_id == fsm->operation_id &&
                event->context == (fsm->operation == BUZZER_OP_STARTUP
                                       ? MEASURE_STARTUP
                                       : MEASURE_SHUTDOWN)) {
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    startup_measurement_failed(fsm, config, commands);
                } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                    complete_shutdown(fsm, commands);
                }
            }
            return;

        case RTEVT_BEEP_FINISHED:
            if (fsm->state != BUZZER_FSM_BEEPING ||
                event->operation_id != fsm->operation_id) {
                return;
            }
            if (fsm->operation == BUZZER_OP_STARTUP) {
                complete_startup(fsm, commands);
            } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                complete_shutdown(fsm, commands);
            } else {
                fsm->state = BUZZER_FSM_IDLE;
                fsm->request_id = 0;
                if (fsm->pending_startup) {
                    begin_startup(fsm, config, fsm->pending_startup_id,
                                  commands);
                }
            }
            return;

        default:
            return;
    }
}
