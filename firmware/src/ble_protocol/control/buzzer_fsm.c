#include "buzzer_fsm.h"

#include <limits.h>
#include <math.h>

#define BUZZER_MIN_SETTLE_MS 1000U
#define BUZZER_PLAYBACK_GRACE_MS 250U

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

static int64_t deadline_after_ms(int64_t now_us, uint64_t duration_ms) {
    if (duration_ms > (uint64_t)(INT64_MAX - now_us) / 1000ULL) {
        return INT64_MAX;
    }
    return now_us + (int64_t)(duration_ms * 1000ULL);
}

static void emit_done(runtime_command_list_t *commands, uint32_t operation_id,
                      buzzer_operation_t operation, int32_t error) {
    runtime_command_t command = runtime_command_make(
        RCMD_EMIT_BUZZER_DONE, operation_id);
    command.buzzer_operation = operation;
    command.error = error;
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

static void set_playback_deadline(buzzer_fsm_t *fsm, int64_t now_us,
                                  uint32_t on_ms, uint16_t off_ms,
                                  uint8_t repeats) {
    uint64_t duration_ms = (uint64_t)on_ms * repeats;
    if (repeats > 1) duration_ms += (uint64_t)off_ms * (repeats - 1U);
    fsm->deadline_us =
        deadline_after_ms(now_us, duration_ms + BUZZER_PLAYBACK_GRACE_MS);
}

static void reset_low_state(buzzer_fsm_t *fsm) {
    fsm->low_pending = false;
    fsm->low_alerted = false;
    fsm->low_since_us = 0;
}

void buzzer_fsm_reset_low_warning(buzzer_fsm_t *fsm) {
    if (fsm) reset_low_state(fsm);
}

static void begin_startup(buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config,
                          uint32_t operation_id, int64_t now_us,
                          runtime_command_list_t *commands) {
    fsm->pending_startup = false;
    fsm->pending_startup_id = 0;
    fsm->operation = BUZZER_OP_STARTUP;
    fsm->operation_id = operation_id;
    fsm->pattern = BUZZER_PATTERN_NONE;
    fsm->request_id = 0;
    fsm->state = BUZZER_FSM_WAIT_SETTLE;
    uint32_t settle_ms = config->settle_ms < BUZZER_MIN_SETTLE_MS
                             ? BUZZER_MIN_SETTLE_MS
                             : config->settle_ms;
    fsm->deadline_us = deadline_after_ms(now_us, settle_ms);

    if (!config->enabled) {
        fsm->state = BUZZER_FSM_IDLE;
        fsm->last_startup_operation_id = operation_id;
        fsm->deadline_us = 0;
        emit_done(commands, operation_id, BUZZER_OP_STARTUP, 0);
        return;
    }

    runtime_command_t timer = runtime_command_make(
        RCMD_START_SETTLE_TIMER, operation_id);
    timer.duration_ms = settle_ms;
    runtime_command_list_push(commands, &timer);
}

static void request_startup_measurement(
    buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config, int64_t now_us,
    runtime_command_list_t *commands) {
    fsm->state = BUZZER_FSM_MEASURING;
    fsm->request_id = runtime_event_next_id();
    uint32_t timeout_ms = config->measure_timeout_ms == 0
                              ? 1U
                              : config->measure_timeout_ms;
    fsm->deadline_us = deadline_after_ms(now_us, timeout_ms);

    runtime_command_t timeout = runtime_command_make(
        RCMD_START_MEASURE_TIMEOUT, fsm->operation_id);
    timeout.request_id = fsm->request_id;
    timeout.context = MEASURE_STARTUP;
    timeout.duration_ms = timeout_ms;
    runtime_command_list_push(commands, &timeout);

    runtime_command_t request = runtime_command_make(
        RCMD_INA_REQUEST, fsm->operation_id);
    request.request_id = fsm->request_id;
    request.context = MEASURE_STARTUP;
    runtime_command_list_push(commands, &request);
}

static void begin_shutdown(buzzer_fsm_t *fsm, const buzzer_fsm_config_t *config,
                           uint32_t operation_id, int64_t now_us,
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
        fsm->deadline_us = 0;
        emit_done(commands, operation_id, BUZZER_OP_SHUTDOWN, 0);
        return;
    }

    fsm->state = BUZZER_FSM_MEASURING;
    fsm->request_id = runtime_event_next_id();
    uint32_t timeout_ms = config->measure_timeout_ms == 0
                              ? 1U
                              : config->measure_timeout_ms;
    fsm->deadline_us = deadline_after_ms(now_us, timeout_ms);

    runtime_command_t timeout = runtime_command_make(
        RCMD_START_MEASURE_TIMEOUT, operation_id);
    timeout.request_id = fsm->request_id;
    timeout.context = MEASURE_SHUTDOWN;
    timeout.duration_ms = timeout_ms;
    runtime_command_list_push(commands, &timeout);

    runtime_command_t request = runtime_command_make(
        RCMD_INA_REQUEST, operation_id);
    request.request_id = fsm->request_id;
    request.context = MEASURE_SHUTDOWN;
    runtime_command_list_push(commands, &request);
}

static void complete_startup(buzzer_fsm_t *fsm,
                             runtime_command_list_t *commands,
                             int32_t error) {
    fsm->state = BUZZER_FSM_IDLE;
    fsm->last_startup_operation_id = fsm->operation_id;
    fsm->request_id = 0;
    fsm->deadline_us = 0;
    fsm->pattern = BUZZER_PATTERN_NONE;
    emit_done(commands, fsm->operation_id, BUZZER_OP_STARTUP, error);
}

static void complete_shutdown(buzzer_fsm_t *fsm,
                              runtime_command_list_t *commands) {
    fsm->state = BUZZER_FSM_IDLE;
    fsm->last_shutdown_operation_id = fsm->operation_id;
    fsm->request_id = 0;
    fsm->deadline_us = 0;
    fsm->pattern = BUZZER_PATTERN_NONE;
    emit_done(commands, fsm->operation_id, BUZZER_OP_SHUTDOWN, 0);
}

static void startup_measurement_failed(buzzer_fsm_t *fsm,
                                       const buzzer_fsm_config_t *config,
                                       int64_t now_us,
                                       runtime_command_list_t *commands) {
    fsm->request_id = 0;
    if (!config->enabled) {
        complete_startup(fsm, commands, 0);
        return;
    }
    fsm->state = BUZZER_FSM_BEEPING;
    fsm->operation = BUZZER_OP_STARTUP;
    fsm->pattern = BUZZER_PATTERN_ERROR;
    set_playback_deadline(fsm, now_us, config->error_on_ms,
                          config->error_off_ms, config->error_repeats);
    play_pattern(commands, fsm->operation_id, BUZZER_PATTERN_ERROR,
                 config->error_on_ms, config->error_off_ms,
                 config->error_repeats);
}

static void finish_shutdown_measurement(
    buzzer_fsm_t *fsm, runtime_command_list_t *commands) {
    complete_shutdown(fsm, commands);
}

static void handle_periodic_sample(buzzer_fsm_t *fsm,
                                   const buzzer_fsm_config_t *config,
                                   const runtime_event_t *event,
                                   int64_t now_us,
                                   runtime_command_list_t *commands) {
    if (fsm->threshold_mv != config->threshold_mv) {
        fsm->threshold_mv = config->threshold_mv;
        reset_low_state(fsm);
    }
    if (!config->load_output_on) {
        reset_low_state(fsm);
        return;
    }
    if (!isfinite(event->voltage_v)) {
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
    set_playback_deadline(fsm, now_us, config->low_ms, 0, 1);
    play_pattern(commands, fsm->operation_id, BUZZER_PATTERN_WARNING,
                 config->low_ms, 0, 1);
}

static bool matching_measurement(const buzzer_fsm_t *fsm,
                                 const runtime_event_t *event) {
    measure_context_t expected = fsm->operation == BUZZER_OP_STARTUP
                                     ? MEASURE_STARTUP
                                     : MEASURE_SHUTDOWN;
    return fsm->state == BUZZER_FSM_MEASURING &&
           event->request_id == fsm->request_id &&
           event->operation_id == fsm->operation_id &&
           event->context == expected;
}

static void handle_measurement_result(
    buzzer_fsm_t *fsm, const runtime_event_t *event,
    const buzzer_fsm_config_t *config, int64_t now_us,
    runtime_command_list_t *commands) {
    if (fsm->operation == BUZZER_OP_STARTUP) {
        if (event->kind != RTEVT_INA_SAMPLE_OK ||
            !isfinite(event->voltage_v)) {
            startup_measurement_failed(fsm, config, now_us, commands);
            return;
        }
        bool low = config->threshold_mv > 0 &&
                   event->voltage_v * 1000.0f <
                       (float)config->threshold_mv;
        uint32_t duration = low ? config->combined_ms : config->button_ms;
        fsm->state = BUZZER_FSM_BEEPING;
        fsm->pattern = low ? BUZZER_PATTERN_LOW_BATTERY
                           : BUZZER_PATTERN_NORMAL;
        set_playback_deadline(fsm, now_us, duration, 0, 1);
        play_pattern(commands, fsm->operation_id, fsm->pattern, duration, 0,
                     1);
        return;
    }

    if (event->kind != RTEVT_INA_SAMPLE_OK ||
        !isfinite(event->voltage_v) || config->threshold_mv == 0 ||
        event->voltage_v * 1000.0f >= (float)config->threshold_mv ||
        !config->enabled || config->low_ms == 0) {
        finish_shutdown_measurement(fsm, commands);
        return;
    }

    fsm->state = BUZZER_FSM_BEEPING;
    fsm->pattern = BUZZER_PATTERN_WARNING;
    set_playback_deadline(fsm, now_us, config->low_ms, 0, 1);
    play_pattern(commands, fsm->operation_id, BUZZER_PATTERN_WARNING,
                 config->low_ms, 0, 1);
}

static void finish_warning(buzzer_fsm_t *fsm,
                           const buzzer_fsm_config_t *config, int64_t now_us,
                           runtime_command_list_t *commands) {
    fsm->state = BUZZER_FSM_IDLE;
    fsm->request_id = 0;
    fsm->deadline_us = 0;
    fsm->pattern = BUZZER_PATTERN_NONE;
    if (fsm->pending_startup) {
        begin_startup(fsm, config, fsm->pending_startup_id, now_us, commands);
    }
}

void buzzer_fsm_step(buzzer_fsm_t *fsm, const runtime_event_t *event,
                     const buzzer_fsm_config_t *config, int64_t now_us,
                     runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    switch (event->kind) {
        case RTEVT_EVENT_BUS_FAULT:
            /* The monotonic deadline remains responsible for finishing work. */
            return;

        case RTEVT_LOAD_GPIO_APPLIED:
            if (event->bool_value &&
                event->operation_id != fsm->last_startup_operation_id &&
                fsm->state == BUZZER_FSM_IDLE) {
                begin_startup(fsm, config, event->operation_id, now_us,
                              commands);
            } else if (event->bool_value &&
                       fsm->state == BUZZER_FSM_BEEPING &&
                       fsm->operation == BUZZER_OP_WARNING) {
                fsm->pending_startup = true;
                fsm->pending_startup_id = event->operation_id;
            }
            return;

        case RTEVT_BUZZER_REQUEST_SHUTDOWN:
            if (event->operation_id == fsm->last_shutdown_operation_id) return;
            if (fsm->state == BUZZER_FSM_BEEPING &&
                fsm->operation == BUZZER_OP_WARNING) {
                runtime_command_t cancel = runtime_command_make(
                    RCMD_BUZZER_CANCEL, fsm->operation_id);
                runtime_command_list_push(commands, &cancel);
                begin_shutdown(fsm, config, event->operation_id, now_us,
                               commands);
            } else if (fsm->state == BUZZER_FSM_IDLE) {
                begin_shutdown(fsm, config, event->operation_id, now_us,
                               commands);
            }
            return;

        case RTEVT_SETTLE_ELAPSED:
            if (fsm->state == BUZZER_FSM_WAIT_SETTLE &&
                event->operation_id == fsm->operation_id &&
                now_us >= fsm->deadline_us) {
                request_startup_measurement(fsm, config, now_us, commands);
            }
            return;

        case RTEVT_INA_SAMPLE_OK:
        case RTEVT_INA_SAMPLE_FAILED:
            if (matching_measurement(fsm, event)) {
                if (fsm->deadline_us == 0 || now_us >= fsm->deadline_us) {
                    if (fsm->operation == BUZZER_OP_STARTUP) {
                        startup_measurement_failed(fsm, config, now_us,
                                                   commands);
                    } else {
                        finish_shutdown_measurement(fsm, commands);
                    }
                    return;
                }
                fsm->deadline_us = 0;
                handle_measurement_result(fsm, event, config, now_us,
                                          commands);
            } else if (fsm->state == BUZZER_FSM_IDLE &&
                       event->context == MEASURE_PERIODIC &&
                       event->kind == RTEVT_INA_SAMPLE_OK) {
                handle_periodic_sample(fsm, config, event, now_us, commands);
            } else if (fsm->state == BUZZER_FSM_IDLE &&
                       event->context == MEASURE_PERIODIC &&
                       event->kind == RTEVT_INA_SAMPLE_FAILED) {
                reset_low_state(fsm);
            }
            return;

        case RTEVT_MEASURE_TIMEOUT:
            if (matching_measurement(fsm, event) &&
                fsm->deadline_us != 0 && now_us >= fsm->deadline_us) {
                fsm->deadline_us = 0;
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    startup_measurement_failed(fsm, config, now_us, commands);
                } else {
                    finish_shutdown_measurement(fsm, commands);
                }
            }
            return;

        case RTEVT_ACTION_FAILED:
            if (event->operation_id != fsm->operation_id) return;
            if (event->command_kind == RCMD_START_SETTLE_TIMER &&
                fsm->state == BUZZER_FSM_WAIT_SETTLE) {
                /* The elapsed-time check replaces a failed timer notification. */
                return;
            }
            if (fsm->state == BUZZER_FSM_MEASURING &&
                event->request_id == fsm->request_id &&
                event->context == (fsm->operation == BUZZER_OP_STARTUP
                                       ? MEASURE_STARTUP
                                       : MEASURE_SHUTDOWN)) {
                if (event->command_kind == RCMD_INA_REQUEST) {
                    fsm->deadline_us = 0;
                    if (fsm->operation == BUZZER_OP_STARTUP) {
                        startup_measurement_failed(fsm, config, now_us,
                                                   commands);
                    } else {
                        finish_shutdown_measurement(fsm, commands);
                    }
                }
                /* Timeout timer failure is covered by the local deadline. */
                return;
            }
            if (fsm->state == BUZZER_FSM_BEEPING &&
                event->command_kind == RCMD_BUZZER_PLAY) {
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    complete_startup(fsm, commands, event->error);
                } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                    complete_shutdown(fsm, commands);
                } else {
                    finish_warning(fsm, config, now_us, commands);
                }
            }
            return;

        case RTEVT_BEEP_FINISHED:
            if (fsm->state != BUZZER_FSM_BEEPING ||
                event->operation_id != fsm->operation_id) {
                return;
            }
            fsm->deadline_us = 0;
            if (fsm->operation == BUZZER_OP_STARTUP) {
                complete_startup(fsm, commands, 0);
            } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                complete_shutdown(fsm, commands);
            } else {
                finish_warning(fsm, config, now_us, commands);
            }
            return;

        case RTEVT_DEADLINE_CHECK:
            if (fsm->deadline_us == 0 || now_us < fsm->deadline_us) return;
            if (fsm->state == BUZZER_FSM_WAIT_SETTLE) {
                request_startup_measurement(fsm, config, now_us, commands);
            } else if (fsm->state == BUZZER_FSM_MEASURING) {
                fsm->deadline_us = 0;
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    startup_measurement_failed(fsm, config, now_us, commands);
                } else {
                    finish_shutdown_measurement(fsm, commands);
                }
            } else if (fsm->state == BUZZER_FSM_BEEPING) {
                runtime_command_t cancel = runtime_command_make(
                    RCMD_BUZZER_CANCEL, fsm->operation_id);
                runtime_command_list_push(commands, &cancel);
                if (fsm->operation == BUZZER_OP_STARTUP) {
                    complete_startup(fsm, commands, 0);
                } else if (fsm->operation == BUZZER_OP_SHUTDOWN) {
                    complete_shutdown(fsm, commands);
                } else {
                    finish_warning(fsm, config, now_us, commands);
                }
            }
            return;

        default:
            return;
    }
}
