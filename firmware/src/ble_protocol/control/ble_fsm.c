#include "ble_fsm.h"

#define BLE_STOP_TIMEOUT_US (10LL * 1000000LL)

const char *ble_fsm_state_name(ble_fsm_state_t state) {
    switch (state) {
        case BLE_FSM_OFF: return "OFF";
        case BLE_FSM_STARTING: return "STARTING";
        case BLE_FSM_ADVERTISING: return "ADVERTISING";
        case BLE_FSM_CONNECTED: return "CONNECTED";
        case BLE_FSM_STOPPING: return "STOPPING";
        case BLE_FSM_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void ble_fsm_init(ble_fsm_t *fsm, int64_t now_us) {
    (void)now_us;
    if (!fsm) return;
    *fsm = (ble_fsm_t) {
        .state = BLE_FSM_OFF,
        .conn_handle = UINT16_MAX,
    };
}

int32_t ble_fsm_remaining_ms(const ble_fsm_t *fsm, int64_t now_us) {
    if (!fsm || fsm->window_deadline_us <= now_us) return 0;
    int64_t remaining = fsm->window_deadline_us - now_us;
    int64_t ms = (remaining + 999) / 1000;
    return ms > INT32_MAX ? INT32_MAX : (int32_t)ms;
}

bool ble_fsm_is_active(const ble_fsm_t *fsm) {
    return fsm && fsm->state != BLE_FSM_OFF;
}

bool ble_fsm_blocks_sleep(const ble_fsm_t *fsm) {
    return ble_fsm_is_active(fsm);
}

static void extend_window(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                          int64_t now_us) {
    fsm->window_deadline_us = now_us + config->window_us;
}

static void begin_start(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                        int64_t now_us, runtime_command_list_t *commands) {
    ble_fsm_init(fsm, now_us);
    fsm->state = BLE_FSM_STARTING;
    extend_window(fsm, config, now_us);
    fsm->operation_id = runtime_event_next_id();

    runtime_command_t command = runtime_command_make(RCMD_BLE_START,
                                                      fsm->operation_id);
    runtime_command_list_push(commands, &command);
}

static void begin_stop(ble_fsm_t *fsm, int64_t now_us,
                       runtime_command_list_t *commands) {
    fsm->state = BLE_FSM_STOPPING;
    fsm->window_deadline_us = 0;
    fsm->adv_request_id = 0;
    fsm->stop_deadline_us = now_us + BLE_STOP_TIMEOUT_US;
    fsm->stop_request_id = runtime_event_next_id();

    runtime_command_t command = runtime_command_make(RCMD_BLE_STOP,
                                                      fsm->operation_id);
    command.request_id = fsm->stop_request_id;
    runtime_command_list_push(commands, &command);
}

static void request_disconnect(const ble_fsm_t *fsm,
                               runtime_command_list_t *commands) {
    runtime_command_t command = runtime_command_make(RCMD_BLE_DISCONNECT,
                                                     fsm->operation_id);
    command.conn_handle = fsm->conn_handle;
    runtime_command_list_push(commands, &command);
}

static void advertise(ble_fsm_t *fsm, bool refresh,
                      runtime_command_list_t *commands) {
    fsm->adv_request_id = runtime_event_next_id();
    runtime_command_t command = runtime_command_make(
        refresh ? RCMD_BLE_REFRESH_ADVERTISING : RCMD_BLE_START_ADVERTISING,
        fsm->operation_id);
    command.request_id = fsm->adv_request_id;
    command.duration_ms = RUNTIME_BLE_ADVERTISING_FOREVER_MS;
    runtime_command_list_push(commands, &command);
}

static bool window_expired(const ble_fsm_t *fsm, int64_t now_us) {
    return (fsm->state == BLE_FSM_STARTING ||
            fsm->state == BLE_FSM_ADVERTISING) &&
           now_us >= fsm->window_deadline_us;
}

static void handle_press(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                         bool pairing, int64_t now_us,
                         runtime_command_list_t *commands) {
    if (window_expired(fsm, now_us)) begin_stop(fsm, now_us, commands);

    switch (fsm->state) {
        case BLE_FSM_OFF:
            begin_start(fsm, config, now_us, commands);
            break;
        case BLE_FSM_STARTING:
            extend_window(fsm, config, now_us);
            break;
        case BLE_FSM_ADVERTISING:
            extend_window(fsm, config, now_us);
            if (pairing) advertise(fsm, true, commands);
            break;
        case BLE_FSM_CONNECTED:
            if (pairing) {
                fsm->restart_requested = true;
                begin_stop(fsm, now_us, commands);
            }
            break;
        case BLE_FSM_STOPPING:
            fsm->restart_requested = true;
            break;
        case BLE_FSM_ERROR:
            fsm->restart_requested = true;
            begin_stop(fsm, now_us, commands);
            break;
    }
}

void ble_fsm_step(ble_fsm_t *fsm, const runtime_event_t *event,
                  const ble_fsm_config_t *config, int64_t now_us,
                  runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    const bool current_operation = event->operation_id == fsm->operation_id;
    const bool current_advertising = current_operation &&
        event->request_id != 0 && event->request_id == fsm->adv_request_id;
    const bool current_stop = current_operation &&
        event->request_id != 0 && event->request_id == fsm->stop_request_id;

    switch (event->kind) {
        case RTEVT_THERMAL_BUTTON_PRESSED:
        case RTEVT_BLE_WINDOW_REQUEST:
        case RTEVT_BLE_PAIRING_REQUEST:
            handle_press(fsm, config, event->kind == RTEVT_BLE_PAIRING_REQUEST,
                         now_us, commands);
            return;

        case RTEVT_BLE_PAIR_TIMEOUT:
        case RTEVT_BLE_ADV_REFRESH:
            if (fsm->state == BLE_FSM_ADVERTISING) {
                if (window_expired(fsm, now_us)) begin_stop(fsm, now_us, commands);
                else advertise(fsm, true, commands);
            } else if (event->kind == RTEVT_BLE_PAIR_TIMEOUT &&
                       fsm->state == BLE_FSM_CONNECTED && event->bool_value) {
                request_disconnect(fsm, commands);
            }
            return;

        case RTEVT_BLE_READY:
            if (fsm->state != BLE_FSM_STARTING || !current_operation) return;
            if (window_expired(fsm, now_us)) {
                begin_stop(fsm, now_us, commands);
            } else {
                fsm->state = BLE_FSM_ADVERTISING;
                advertise(fsm, false, commands);
            }
            return;

        case RTEVT_ADV_COMPLETE:
            if (fsm->state == BLE_FSM_ADVERTISING && current_advertising) {
                begin_stop(fsm, now_us, commands);
            }
            return;

        case RTEVT_BLE_HOST_RESET:
            if (current_operation &&
                (fsm->state == BLE_FSM_STARTING || fsm->state == BLE_FSM_ADVERTISING ||
                 fsm->state == BLE_FSM_CONNECTED)) {
                begin_stop(fsm, now_us, commands);
            }
            return;

        case RTEVT_BLE_START_FAILED:
            if ((fsm->state == BLE_FSM_STARTING && current_operation) ||
                (fsm->state == BLE_FSM_ADVERTISING && current_advertising)) {
                begin_stop(fsm, now_us, commands);
            }
            return;

        case RTEVT_CLIENT_CONNECTED:
            if (fsm->state != BLE_FSM_ADVERTISING || !current_operation ||
                event->request_id == 0 || event->conn_handle == UINT16_MAX) {
                return;
            }
            /* A link can arrive from the preceding advertisement during refresh. */
            if (window_expired(fsm, now_us)) {
                begin_stop(fsm, now_us, commands);
            } else {
                fsm->state = BLE_FSM_CONNECTED;
                fsm->conn_handle = event->conn_handle;
                fsm->window_deadline_us = 0;
                fsm->adv_request_id = 0;
            }
            return;

        case RTEVT_CLIENT_DISCONNECTED:
            if (fsm->state == BLE_FSM_CONNECTED && current_operation &&
                event->conn_handle == fsm->conn_handle) {
                fsm->conn_handle = UINT16_MAX;
                begin_stop(fsm, now_us, commands);
            }
            return;

        case RTEVT_BLE_PAIR_FINISH_DISCONNECT:
            if (fsm->state == BLE_FSM_CONNECTED && current_operation &&
                event->conn_handle == fsm->conn_handle) {
                request_disconnect(fsm, commands);
            }
            return;

        case RTEVT_BLE_STOPPED:
            if ((fsm->state == BLE_FSM_STOPPING || fsm->state == BLE_FSM_ERROR) &&
                current_stop) {
                if (fsm->restart_requested) begin_start(fsm, config, now_us, commands);
                else ble_fsm_init(fsm, now_us);
            }
            return;

        case RTEVT_BLE_STOP_FAILED:
            if (fsm->state == BLE_FSM_STOPPING && current_stop) {
                fsm->state = BLE_FSM_ERROR;
            }
            return;

        case RTEVT_DEADLINE_CHECK:
            if (window_expired(fsm, now_us)) begin_stop(fsm, now_us, commands);
            if (fsm->state == BLE_FSM_STOPPING && now_us >= fsm->stop_deadline_us) {
                fsm->state = BLE_FSM_ERROR;
            }
            return;

        case RTEVT_ACTION_FAILED:
            if (event->command_kind == RCMD_BLE_STOP &&
                fsm->state == BLE_FSM_STOPPING && current_stop) {
                fsm->state = BLE_FSM_ERROR;
            } else if ((event->command_kind == RCMD_BLE_START &&
                        fsm->state == BLE_FSM_STARTING && current_operation) ||
                       ((event->command_kind == RCMD_BLE_START_ADVERTISING ||
                         event->command_kind == RCMD_BLE_REFRESH_ADVERTISING) &&
                        fsm->state == BLE_FSM_ADVERTISING && current_advertising)) {
                begin_stop(fsm, now_us, commands);
            }
            return;

        default:
            return;
    }
}
