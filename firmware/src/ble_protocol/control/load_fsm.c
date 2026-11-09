#include "load_fsm.h"

#include <math.h>

#define LOAD_OFF_MAX_RETRIES 3U
#define LOAD_ON_MAX_RETRIES 3U
#define LOAD_VERIFY_MAX_RETRIES 3U
#define LOAD_ACTION_ACK_TIMEOUT_US 1000000LL
#define LOAD_CHARGING_RETRY_DELAY_US 1000000LL

const char *load_fsm_state_name(load_fsm_state_t state) {
    switch (state) {
        case LOAD_FSM_IDLE: return "IDLE";
        case LOAD_FSM_STARTING: return "STARTING";
        case LOAD_FSM_WORKING: return "WORKING";
        case LOAD_FSM_STOPPING: return "STOPPING";
        case LOAD_FSM_ERROR: return "ERROR";
        case LOAD_FSM_NEED_CHARGING: return "NEED_CHARGING";
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

void load_fsm_restore_charging_confirmation(load_fsm_t *fsm, bool pending,
                                            int64_t since_us) {
    if (!fsm) return;
    fsm->low_voltage_current_pending =
        fsm->state == LOAD_FSM_WORKING && pending;
    fsm->low_voltage_current_since_us =
        fsm->low_voltage_current_pending ? since_us : 0;
}

static void command_output(load_fsm_t *fsm, runtime_command_list_t *commands,
                           runtime_command_kind_t kind) {
    runtime_command_t command = runtime_command_make(kind, fsm->operation_id);
    command.bool_value = kind == RCMD_LOAD_OUTPUT_ON;
    runtime_command_list_push(commands, &command);
}

static int64_t add_ms(int64_t now_us, uint32_t duration_ms) {
    if (now_us < 0 ||
        (uint64_t)duration_ms > (uint64_t)(INT64_MAX - now_us) / 1000ULL) {
        return INT64_MAX;
    }
    return now_us + (int64_t)duration_ms * 1000LL;
}

static void charging_start_measurement(load_fsm_t *fsm,
                                       const load_fsm_config_t *config,
                                       int64_t now_us,
                                       runtime_command_list_t *commands) {
    fsm->charging_phase = LOAD_CHARGE_MEASURING;
    fsm->charging_measure_pending = true;
    fsm->charging_request_id = runtime_event_next_id();
    fsm->charging_deadline_us =
        config->measure_timeout_us > 0
            ? now_us + config->measure_timeout_us
            : now_us + 1;

    runtime_command_t timeout = runtime_command_make(
        RCMD_START_MEASURE_TIMEOUT, fsm->operation_id);
    timeout.request_id = fsm->charging_request_id;
    timeout.context = MEASURE_NEED_CHARGING;
    timeout.duration_ms = config->measure_timeout_us > 0
                              ? (uint32_t)(config->measure_timeout_us / 1000LL)
                              : 1U;
    runtime_command_list_push(commands, &timeout);

    runtime_command_t request = runtime_command_make(
        RCMD_INA_REQUEST, fsm->operation_id);
    request.request_id = fsm->charging_request_id;
    request.context = MEASURE_NEED_CHARGING;
    runtime_command_list_push(commands, &request);
}

static void charging_retry_measurement(load_fsm_t *fsm, int64_t now_us) {
    fsm->charging_measure_pending = false;
    fsm->charging_deadline_us = now_us + LOAD_CHARGING_RETRY_DELAY_US;
    fsm->charging_phase = LOAD_CHARGE_RETRY_MEASUREMENT;
}

static void begin_need_charging(load_fsm_t *fsm, int64_t now_us,
                                runtime_command_list_t *commands) {
    fsm->state = LOAD_FSM_NEED_CHARGING;
    fsm->low_current_pending = false;
    fsm->low_voltage_current_pending = false;
    fsm->charging_measure_pending = false;
    fsm->charging_deadline_us = 0;
    fsm->charging_phase = LOAD_CHARGE_TURNING_ON;
    fsm->operation_id = runtime_event_next_id();
    fsm->on_retries = 0;
    fsm->off_retries = 0;
    fsm->verification_retries = 0;
    fsm->verification_pending = false;
    fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
    command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
}

static void charging_begin_off(load_fsm_t *fsm, int64_t now_us,
                               runtime_command_list_t *commands) {
    fsm->operation_id = runtime_event_next_id();
    fsm->charging_phase = LOAD_CHARGE_TURNING_OFF;
    fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
    fsm->off_retries = 0;
    fsm->verification_retries = 0;
    fsm->verification_pending = false;
    command_output(fsm, commands, RCMD_LOAD_OUTPUT_OFF);
}

static void charging_recover_to_working(load_fsm_t *fsm,
                                        runtime_command_list_t *commands) {
    if (fsm->charging_measure_pending) {
        runtime_command_t cancel = runtime_command_make(
            RCMD_CANCEL_MEASURE_TIMEOUT, fsm->operation_id);
        runtime_command_list_push(commands, &cancel);
    }
    fsm->state = LOAD_FSM_WORKING;
    fsm->operation_id = runtime_event_next_id();
    fsm->charging_phase = LOAD_CHARGE_NONE;
    fsm->charging_deadline_us = 0;
    fsm->charging_measure_pending = false;
    fsm->charging_request_id = 0;
    fsm->action_deadline_us = 0;
    fsm->feedback_deadline_us = 0;
    fsm->low_current_pending = false;
    fsm->low_current_since_us = 0;
    fsm->low_voltage_current_pending = false;
    fsm->low_voltage_current_since_us = 0;
    fsm->on_retries = 0;
    fsm->off_retries = 0;
    fsm->verification_retries = 0;
    fsm->verification_pending = false;
}

static void charging_retry_load_on(load_fsm_t *fsm, int64_t now_us,
                                   runtime_command_list_t *commands) {
    fsm->verification_pending = false;
    fsm->verification_retries = 0;
    if (fsm->on_retries < LOAD_ON_MAX_RETRIES) {
        ++fsm->on_retries;
        fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
        command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
    } else {
        fsm->state = LOAD_FSM_ERROR;
        fsm->output_on = true;
        fsm->action_deadline_us = 0;
        fsm->charging_deadline_us = 0;
    }
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
    fsm->low_voltage_current_pending = false;
    fsm->charging_phase = LOAD_CHARGE_NONE;
    fsm->charging_deadline_us = 0;
    fsm->charging_measure_pending = false;
    fsm->action_deadline_us = now_us + LOAD_ACTION_ACK_TIMEOUT_US;
    fsm->feedback_deadline_us = 0;
    command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
}

static void begin_shutdown(load_fsm_t *fsm, runtime_command_list_t *commands) {
    fsm->operation_id = runtime_event_next_id();
    fsm->shutdown_operation_id = fsm->operation_id;
    fsm->state = LOAD_FSM_STOPPING;
    fsm->low_current_pending = false;
    fsm->low_voltage_current_pending = false;
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
    if (fsm->state == LOAD_FSM_NEED_CHARGING) {
        return (fsm->charging_phase == LOAD_CHARGE_TURNING_ON &&
                event->command_kind == RCMD_LOAD_OUTPUT_ON) ||
               (fsm->charging_phase == LOAD_CHARGE_TURNING_OFF &&
                event->command_kind == RCMD_LOAD_OUTPUT_OFF);
    }
    if (fsm->state == LOAD_FSM_STOPPING && fsm->off_requested) {
        return event->command_kind == RCMD_LOAD_OUTPUT_OFF;
    }
    return false;
}

static void handle_gpio_verification(load_fsm_t *fsm,
                                     const runtime_event_t *event,
                                     const load_fsm_config_t *config,
                                     int64_t now_us,
                                     runtime_command_list_t *commands) {
    if (!is_current_gpio_verification(fsm, event)) return;
    fsm->verification_pending = false;
    fsm->action_deadline_us = 0;

    if (event->error == 0 && event->bool_value) {
        fsm->verification_retries = 0;
        if (fsm->state == LOAD_FSM_STARTING) {
            fsm->output_on = true;
        } else if (fsm->state == LOAD_FSM_NEED_CHARGING) {
            fsm->output_on = event->command_kind == RCMD_LOAD_OUTPUT_ON;
            fsm->action_deadline_us = 0;
            fsm->verification_retries = 0;
            if (fsm->output_on) {
                fsm->charging_phase = LOAD_CHARGE_ON;
                fsm->charging_deadline_us =
                    add_ms(now_us, config->charging_blink_interval_ms);
            } else {
                fsm->charging_phase = LOAD_CHARGE_OFF;
                fsm->charging_deadline_us =
                    add_ms(now_us, config->charging_blink_interval_ms);
            }
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
    } else if (fsm->state == LOAD_FSM_NEED_CHARGING) {
        if (event->command_kind == RCMD_LOAD_OUTPUT_ON) {
            charging_retry_load_on(fsm, now_us, commands);
        } else {
            fail_load_off(fsm, now_us, commands);
        }
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
        case RTEVT_WHEEL_MOTION_CONFIRMED:
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
            } else if (fsm->state == LOAD_FSM_NEED_CHARGING) {
                if (event->bool_value &&
                    fsm->charging_phase == LOAD_CHARGE_TURNING_ON) {
                    fsm->output_on = true;
                    fsm->charging_phase = LOAD_CHARGE_ON;
                    fsm->charging_deadline_us =
                        add_ms(now_us, config->charging_blink_interval_ms);
                    fsm->action_deadline_us = 0;
                    fsm->verification_pending = false;
                    fsm->verification_retries = 0;
                    fsm->on_retries = 0;
                } else if (!event->bool_value &&
                           fsm->charging_phase == LOAD_CHARGE_TURNING_OFF) {
                    fsm->output_on = false;
                    fsm->charging_phase = LOAD_CHARGE_OFF;
                    fsm->charging_deadline_us =
                        add_ms(now_us, config->charging_blink_interval_ms);
                    fsm->action_deadline_us = 0;
                    fsm->verification_pending = false;
                    fsm->verification_retries = 0;
                    fsm->off_retries = 0;
                } else {
                    return;
                }
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
            } else if (fsm->state == LOAD_FSM_NEED_CHARGING &&
                       event->bool_value &&
                       fsm->charging_phase == LOAD_CHARGE_TURNING_ON) {
                charging_retry_load_on(fsm, now_us, commands);
            } else if (fsm->state == LOAD_FSM_NEED_CHARGING &&
                       !event->bool_value &&
                       fsm->charging_phase == LOAD_CHARGE_TURNING_OFF) {
                fail_load_off(fsm, now_us, commands);
            } else if (fsm->state == LOAD_FSM_STOPPING &&
                       !event->bool_value && fsm->off_requested) {
                fail_load_off(fsm, now_us, commands);
            }
            return;

        case RTEVT_LOAD_GPIO_VERIFIED:
            handle_gpio_verification(fsm, event, config, now_us, commands);
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
                handle_gpio_verification(fsm, &failed_check, config, now_us,
                                         commands);
                return;
            }
            if (fsm->state == LOAD_FSM_NEED_CHARGING &&
                (event->command_kind == RCMD_INA_REQUEST ||
                 event->command_kind == RCMD_START_MEASURE_TIMEOUT) &&
                event->context == MEASURE_NEED_CHARGING &&
                fsm->charging_measure_pending &&
                event->request_id == fsm->charging_request_id) {
                charging_retry_measurement(fsm, now_us);
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
            } else if (event->command_kind == RCMD_LOAD_OUTPUT_ON &&
                       fsm->state == LOAD_FSM_NEED_CHARGING &&
                       fsm->charging_phase == LOAD_CHARGE_TURNING_ON) {
                charging_retry_load_on(fsm, now_us, commands);
            } else if (event->command_kind == RCMD_LOAD_OUTPUT_OFF &&
                       fsm->state == LOAD_FSM_NEED_CHARGING &&
                       fsm->charging_phase == LOAD_CHARGE_TURNING_OFF) {
                fail_load_off(fsm, now_us, commands);
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
            if (fsm->state == LOAD_FSM_NEED_CHARGING &&
                event->context == MEASURE_NEED_CHARGING &&
                fsm->charging_measure_pending &&
                event->request_id == fsm->charging_request_id) {
                if (!isfinite(event->voltage_v) ||
                    event->voltage_v < 0.0f ||
                    !isfinite(event->current_ma)) {
                    charging_retry_measurement(fsm, now_us);
                    return;
                }
                fsm->charging_measure_pending = false;
                fsm->charging_deadline_us = 0;
                /* Use the current threshold, including changes made over BLE.
                 * Only a fresh ON-phase sample may end charging indication. */
                if (event->voltage_v >= config->low_voltage_v ||
                    fabsf(event->current_ma) > config->low_current_ma) {
                    charging_recover_to_working(fsm, commands);
                } else {
                    charging_begin_off(fsm, now_us, commands);
                }
                return;
            }
            if (fsm->state != LOAD_FSM_WORKING ||
                event->context != MEASURE_PERIODIC) {
                return;
            }
            if (!isfinite(event->voltage_v) || event->voltage_v < 0.0f ||
                !isfinite(event->current_ma)) {
                fsm->low_current_pending = false;
                fsm->low_voltage_current_pending = false;
                return;
            }
            {
                bool current_low =
                    fabsf(event->current_ma) < config->low_current_ma;
                bool voltage_low = event->voltage_v < config->low_voltage_v;
                if (voltage_low && current_low) {
                    fsm->low_current_pending = false;
                    if (!fsm->low_voltage_current_pending ||
                        event->timestamp_us <
                            fsm->low_voltage_current_since_us) {
                        fsm->low_voltage_current_pending = true;
                        fsm->low_voltage_current_since_us =
                            event->timestamp_us;
                    } else if (config->need_charging_confirm_us > 0 &&
                               event->timestamp_us -
                                       fsm->low_voltage_current_since_us >=
                                   config->need_charging_confirm_us) {
                        begin_need_charging(fsm, now_us, commands);
                    }
                    return;
                }

                fsm->low_voltage_current_pending = false;
                if (!current_low) {
                    fsm->low_current_pending = false;
                    return;
                }
                if (!fsm->low_current_pending ||
                    event->timestamp_us < fsm->low_current_since_us) {
                    fsm->low_current_pending = true;
                    fsm->low_current_since_us = event->timestamp_us;
                } else if (event->timestamp_us -
                               fsm->low_current_since_us >=
                           config->low_current_timeout_us) {
                    begin_shutdown(fsm, commands);
                }
            }
            return;

        case RTEVT_INA_SAMPLE_FAILED:
            if (fsm->state == LOAD_FSM_WORKING &&
                event->context == MEASURE_PERIODIC) {
                /* Invalid current is unknown, never low.  Restart the interval. */
                fsm->low_current_pending = false;
                fsm->low_voltage_current_pending = false;
            } else if (fsm->state == LOAD_FSM_NEED_CHARGING &&
                       event->context == MEASURE_NEED_CHARGING &&
                       fsm->charging_measure_pending &&
                       event->request_id == fsm->charging_request_id) {
                charging_retry_measurement(fsm, now_us);
            }
            return;

        case RTEVT_MEASURE_TIMEOUT:
            if (fsm->state == LOAD_FSM_NEED_CHARGING &&
                event->context == MEASURE_NEED_CHARGING &&
                fsm->charging_measure_pending &&
                event->request_id == fsm->charging_request_id &&
                now_us >= fsm->charging_deadline_us) {
                charging_retry_measurement(fsm, now_us);
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
            if (fsm->state == LOAD_FSM_NEED_CHARGING) {
                if (fsm->charging_deadline_us != 0 &&
                    now_us >= fsm->charging_deadline_us) {
                    if (fsm->charging_phase == LOAD_CHARGE_ON) {
                        charging_start_measurement(fsm, config, now_us,
                                                   commands);
                    } else if (fsm->charging_phase == LOAD_CHARGE_OFF) {
                        fsm->operation_id = runtime_event_next_id();
                        fsm->charging_phase = LOAD_CHARGE_TURNING_ON;
                        fsm->charging_deadline_us = 0;
                        fsm->action_deadline_us =
                            now_us + LOAD_ACTION_ACK_TIMEOUT_US;
                        fsm->on_retries = 0;
                        command_output(fsm, commands, RCMD_LOAD_OUTPUT_ON);
                    } else if (fsm->charging_phase ==
                               LOAD_CHARGE_RETRY_MEASUREMENT) {
                        charging_start_measurement(fsm, config, now_us,
                                                   commands);
                    } else if (fsm->charging_phase ==
                                   LOAD_CHARGE_MEASURING &&
                               fsm->charging_measure_pending) {
                        charging_retry_measurement(fsm, now_us);
                    }
                    return;
                }
                if (fsm->action_deadline_us == 0 ||
                    now_us < fsm->action_deadline_us) {
                    return;
                }
                if (fsm->charging_phase == LOAD_CHARGE_TURNING_ON) {
                    if (fsm->verification_pending) {
                        if (fsm->verification_retries <
                            LOAD_VERIFY_MAX_RETRIES) {
                            ++fsm->verification_retries;
                            request_gpio_verification(fsm, true, now_us,
                                                      commands);
                        } else {
                            charging_retry_load_on(fsm, now_us, commands);
                        }
                    } else {
                        request_gpio_verification(fsm, true, now_us,
                                                  commands);
                    }
                } else if (fsm->charging_phase ==
                           LOAD_CHARGE_TURNING_OFF) {
                    if (fsm->verification_pending) {
                        if (fsm->verification_retries <
                            LOAD_VERIFY_MAX_RETRIES) {
                            ++fsm->verification_retries;
                            request_gpio_verification(fsm, false, now_us,
                                                      commands);
                        } else {
                            fail_load_off(fsm, now_us, commands);
                        }
                    } else {
                        request_gpio_verification(fsm, false, now_us,
                                                  commands);
                    }
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
