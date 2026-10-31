#include "runtime_core.h"

#include <string.h>

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
    ble_fsm_init(&core->ble, 0);
}

void runtime_core_step(runtime_core_t *core, const runtime_event_t *event,
                       int64_t now_us, runtime_command_list_t *commands) {
    if (!core || !event || !commands) return;

    /* Deterministic order: Load may request a Buzzer operation, but that
     * request is delivered as a separate event by the dispatcher. */
    load_fsm_step(&core->load, event, &core->load_config, commands);
    buzzer_fsm_step(&core->buzzer, event, &core->buzzer_config, commands);
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
