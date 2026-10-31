#include "power_manager.h"

const char *power_manager_block_reason_name(power_sleep_block_reason_t reason) {
    switch (reason) {
        case POWER_SLEEP_ALLOWED: return "allowed";
        case POWER_BLOCK_BLE: return "ble_active";
        case POWER_BLOCK_BLE_CONNECTED: return "ble_connected";
        case POWER_BLOCK_BLE_RADIO: return "ble_radio_not_quiet";
        case POWER_BLOCK_LOAD_TRANSITION: return "load_transition";
        case POWER_BLOCK_BUZZER: return "buzzer_active";
        case POWER_BLOCK_ACTION: return "action_pending";
        case POWER_BLOCK_EVENT: return "event_pending";
        case POWER_BLOCK_CRITICAL: return "critical_pending";
        default: return "unknown";
    }
}

power_sleep_block_reason_t power_manager_sleep_block_reason(
    const power_snapshot_t *snapshot) {
    if (!snapshot) return POWER_BLOCK_CRITICAL;
    if (snapshot->ble_connected) return POWER_BLOCK_BLE_CONNECTED;
    if (snapshot->ble_state != BLE_FSM_OFF) return POWER_BLOCK_BLE;
    if (!snapshot->ble_radio_quiet) return POWER_BLOCK_BLE_RADIO;
    if (snapshot->load_state == LOAD_FSM_STARTING ||
        snapshot->load_state == LOAD_FSM_STOPPING) {
        return POWER_BLOCK_LOAD_TRANSITION;
    }
    if (snapshot->buzzer_state != BUZZER_FSM_IDLE) return POWER_BLOCK_BUZZER;
    if (snapshot->action_pending) return POWER_BLOCK_ACTION;
    if (snapshot->event_pending) return POWER_BLOCK_EVENT;
    if (snapshot->critical_pending) return POWER_BLOCK_CRITICAL;
    /* LOAD_FSM_WORKING is deliberately sleepable; the output is held by RTC
     * GPIO hold and restored on the next wake. */
    return POWER_SLEEP_ALLOWED;
}

bool power_manager_can_sleep(const power_snapshot_t *snapshot) {
    return power_manager_sleep_block_reason(snapshot) == POWER_SLEEP_ALLOWED;
}
