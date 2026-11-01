#include "runtime_core.h"

#include <string.h>

#define RUNTIME_STARTUP_FEEDBACK_GRACE_MS 500ULL
#define RUNTIME_SHUTDOWN_FEEDBACK_GRACE_MS 500ULL
#define RUNTIME_BUZZER_DEADLINE_GRACE_MS 250ULL

/* The Load fallback outlives every Buzzer phase and its runtime poll delays. */
static int64_t add_milliseconds(int64_t now_us, uint64_t duration_ms) {
    if (now_us < 0 ||
        duration_ms > (uint64_t)(INT64_MAX - now_us) / 1000ULL) {
        return INT64_MAX;
    }
    return now_us + (int64_t)(duration_ms * 1000ULL);
}

static uint64_t startup_feedback_timeout_ms(
    const buzzer_fsm_config_t *config) {
    uint64_t settle_ms = config->settle_ms < BUZZER_SETTLE_MS
                             ? BUZZER_SETTLE_MS
                             : config->settle_ms;
    uint64_t measure_ms = config->measure_timeout_ms == 0
                              ? 1U
                              : config->measure_timeout_ms;
    uint64_t error_ms = (uint64_t)config->error_on_ms *
                        config->error_repeats;
    if (config->error_repeats > 1) {
        error_ms += (uint64_t)config->error_off_ms *
                    (config->error_repeats - 1U);
    }
    uint64_t playback_ms = config->button_ms;
    if (config->combined_ms > playback_ms) playback_ms = config->combined_ms;
    if (error_ms > playback_ms) playback_ms = error_ms;
    uint64_t warning_ms = config->low_ms == 0
                              ? 0
                              : (uint64_t)config->low_ms +
                                    RUNTIME_BUZZER_DEADLINE_GRACE_MS;
    return settle_ms + measure_ms + playback_ms +
           RUNTIME_BUZZER_DEADLINE_GRACE_MS +
           RUNTIME_STARTUP_FEEDBACK_GRACE_MS + warning_ms;
}

static uint64_t shutdown_feedback_timeout_ms(
    const buzzer_fsm_config_t *config) {
    uint64_t measure_ms = config->measure_timeout_ms == 0
                              ? 1U
                              : config->measure_timeout_ms;
    return measure_ms + config->low_ms +
           RUNTIME_BUZZER_DEADLINE_GRACE_MS +
           RUNTIME_SHUTDOWN_FEEDBACK_GRACE_MS;
}

void runtime_core_init(runtime_core_t *core,
                       const load_fsm_config_t *load_config,
                       const buzzer_fsm_config_t *buzzer_config,
                       const ble_fsm_config_t *ble_config,
                       bool restored_working,
                       bool load_low_pending,
                       int64_t load_low_since_us,
                       bool buzzer_low_pending,
                       bool buzzer_low_alerted,
                       int64_t buzzer_low_since_us) {
    if (!core) return;
    memset(core, 0, sizeof(*core));
    if (load_config) core->load_config = *load_config;
    if (buzzer_config) core->buzzer_config = *buzzer_config;
    if (ble_config) core->ble_config = *ble_config;

    load_fsm_init(&core->load, restored_working, load_low_pending,
                  load_low_since_us);
    buzzer_fsm_init(&core->buzzer, &core->buzzer_config, buzzer_low_pending,
                    buzzer_low_alerted, buzzer_low_since_us);
    if (!core->load.output_on) {
        buzzer_fsm_reset_low_warning(&core->buzzer);
    }
    ble_fsm_init(&core->ble, 0);
}

void runtime_core_step(runtime_core_t *core, const runtime_event_t *event,
                       int64_t now_us, runtime_command_list_t *commands) {
    if (!core || !event || !commands) return;

    bool load_on_confirmed =
        (event->kind == RTEVT_LOAD_GPIO_APPLIED && event->bool_value) ||
        (event->kind == RTEVT_LOAD_GPIO_VERIFIED &&
         event->command_kind == RCMD_LOAD_OUTPUT_ON &&
         event->error == 0 && event->bool_value);
    bool new_load_on_ack =
        load_on_confirmed &&
        core->load.state == LOAD_FSM_STARTING && !core->load.output_on &&
        event->operation_id == core->load.operation_id;
    bool current_shutdown_request =
        event->kind == RTEVT_BUZZER_REQUEST_SHUTDOWN &&
        core->load.state == LOAD_FSM_STOPPING &&
        event->operation_id == core->load.shutdown_operation_id;
    load_fsm_state_t previous_load_state = core->load.state;
    bool previous_output_on = core->load.output_on;

    load_fsm_step(&core->load, event, &core->load_config, now_us, commands);
    if (previous_output_on && !core->load.output_on) {
        buzzer_fsm_reset_low_warning(&core->buzzer);
    }
    if (new_load_on_ack && core->load.state == LOAD_FSM_STARTING) {
        core->load.feedback_deadline_us = add_milliseconds(
            now_us, startup_feedback_timeout_ms(&core->buzzer_config));
    } else if (previous_load_state == LOAD_FSM_WORKING &&
               core->load.state == LOAD_FSM_STOPPING) {
        core->load.feedback_deadline_us = add_milliseconds(
            now_us, shutdown_feedback_timeout_ms(&core->buzzer_config));
    }

    /* Only a newly accepted physical ON result may begin startup feedback.
     * Shutdown requests are correlated with the Load operation as well. */
    runtime_event_t buzzer_event = *event;
    if (new_load_on_ack &&
        event->kind == RTEVT_LOAD_GPIO_VERIFIED) {
        buzzer_event.kind = RTEVT_LOAD_GPIO_APPLIED;
        buzzer_event.bool_value = true;
    }

    if ((event->kind != RTEVT_LOAD_GPIO_APPLIED || new_load_on_ack) &&
        (event->kind != RTEVT_BUZZER_REQUEST_SHUTDOWN ||
         current_shutdown_request)) {
        buzzer_fsm_config_t buzzer_config = core->buzzer_config;
        buzzer_config.load_output_on = core->load.output_on;
        buzzer_fsm_step(&core->buzzer, &buzzer_event, &buzzer_config,
                        now_us, commands);
    }
    ble_fsm_step(&core->ble, event, &core->ble_config, now_us, commands);
}

bool runtime_core_command_event(const runtime_command_t *command,
                                runtime_event_t *event) {
    if (!command || !event) return false;

    switch (command->kind) {
        case RCMD_BUZZER_REQUEST_SHUTDOWN:
            *event = runtime_event_make(RTEVT_BUZZER_REQUEST_SHUTDOWN, 0);
            event->operation_id = command->operation_id;
            return true;

        case RCMD_EMIT_BUZZER_DONE:
            *event = runtime_event_make(RTEVT_BUZZER_DONE, 0);
            event->operation_id = command->operation_id;
            event->buzzer_operation = command->buzzer_operation;
            event->error = command->error;
            return true;

        default:
            return false;
    }
}
