#include "ble_fsm.h"

#define BLE_CONNECTION_TIMEOUT_US (600LL * 1000000LL)

const char *ble_fsm_state_name(ble_fsm_state_t state) {
    switch (state) {
        case BLE_FSM_OFF: return "OFF";
        case BLE_FSM_STARTING: return "STARTING";
        case BLE_FSM_ADVERTISING: return "ADVERTISING";
        case BLE_FSM_CONNECTED: return "CONNECTED";
        case BLE_FSM_READY_TO_SLEEP: return "READY_TO_SLEEP";
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
    return fsm && (fsm->state == BLE_FSM_STARTING ||
                   fsm->state == BLE_FSM_ADVERTISING ||
                   fsm->state == BLE_FSM_CONNECTED);
}

bool ble_fsm_blocks_sleep(const ble_fsm_t *fsm) {
    return ble_fsm_is_active(fsm);
}

static void ready_to_sleep(ble_fsm_t *fsm) {
    fsm->state = BLE_FSM_READY_TO_SLEEP;
    fsm->window_deadline_us = 0;
    fsm->startup_deadline_us = 0;
    fsm->connection_deadline_us = 0;
    fsm->conn_handle = UINT16_MAX;
    fsm->adv_request_id = 0;
}

static void begin_start(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                        int64_t now_us, runtime_command_list_t *commands) {
    fsm->state = BLE_FSM_STARTING;
    fsm->window_deadline_us = now_us + config->window_us;
    fsm->startup_deadline_us = fsm->window_deadline_us;
    fsm->connection_deadline_us = 0;
    fsm->operation_id = runtime_event_next_id();
    fsm->conn_handle = UINT16_MAX;
    fsm->adv_request_id = 0;

    runtime_command_t command = runtime_command_make(
        RCMD_BLE_START, fsm->operation_id);
    command.duration_ms = (uint32_t)(config->window_us / 1000);
    runtime_command_list_push(commands, &command);
}

static void extend_window(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                          int64_t now_us) {
    fsm->window_deadline_us = now_us + config->window_us;
}

static bool issue_advertising(ble_fsm_t *fsm, int64_t now_us, bool refresh,
                              runtime_command_list_t *commands) {
    int32_t remaining = ble_fsm_remaining_ms(fsm, now_us);
    if (remaining <= 0) return false;

    fsm->adv_request_id = runtime_event_next_id();

    runtime_command_t command = runtime_command_make(
        refresh ? RCMD_BLE_REFRESH_ADVERTISING : RCMD_BLE_START_ADVERTISING,
        fsm->operation_id);
    command.request_id = fsm->adv_request_id;
    command.duration_ms = RUNTIME_BLE_ADVERTISING_FOREVER_MS;
    runtime_command_list_push(commands, &command);
    return true;
}

static void check_deadline(ble_fsm_t *fsm, int64_t now_us) {
    if (fsm->state == BLE_FSM_STARTING &&
        fsm->startup_deadline_us > 0 &&
        now_us >= fsm->startup_deadline_us) {
        ready_to_sleep(fsm);
    } else if (fsm->state == BLE_FSM_ADVERTISING &&
               fsm->window_deadline_us > 0 &&
               now_us >= fsm->window_deadline_us) {
        ready_to_sleep(fsm);
    } else if (fsm->state == BLE_FSM_CONNECTED &&
               fsm->connection_deadline_us > 0 &&
               now_us >= fsm->connection_deadline_us) {
        ready_to_sleep(fsm);
    }
}

void ble_fsm_step(ble_fsm_t *fsm, const runtime_event_t *event,
                  const ble_fsm_config_t *config, int64_t now_us,
                  runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    switch (event->kind) {
        case RTEVT_THERMAL_BUTTON_PRESSED:
        case RTEVT_BLE_WINDOW_REQUEST:
            switch (fsm->state) {
                case BLE_FSM_OFF:
                    begin_start(fsm, config, now_us, commands);
                    break;
                case BLE_FSM_STARTING:
                    extend_window(fsm, config, now_us);
                    break;
                case BLE_FSM_ADVERTISING:
                    extend_window(fsm, config, now_us);
                    break;
                case BLE_FSM_CONNECTED:
                case BLE_FSM_READY_TO_SLEEP:
                default:
                    break;
            }
            return;

        case RTEVT_BLE_PAIRING_REQUEST:
            switch (fsm->state) {
                case BLE_FSM_OFF:
                    begin_start(fsm, config, now_us, commands);
                    break;
                case BLE_FSM_STARTING:
                    extend_window(fsm, config, now_us);
                    break;
                case BLE_FSM_ADVERTISING:
                    extend_window(fsm, config, now_us);
                    if (!issue_advertising(fsm, now_us, true, commands)) {
                        ready_to_sleep(fsm);
                    }
                    break;
                case BLE_FSM_CONNECTED: {
                    runtime_command_t disconnect = runtime_command_make(
                        RCMD_BLE_DISCONNECT, fsm->operation_id);
                    disconnect.conn_handle = fsm->conn_handle;
                    runtime_command_list_push(commands, &disconnect);
                    break;
                }
                case BLE_FSM_READY_TO_SLEEP:
                default:
                    break;
            }
            return;

        case RTEVT_BLE_PAIR_TIMEOUT:
            if (fsm->state == BLE_FSM_ADVERTISING) {
                check_deadline(fsm, now_us);
                if (fsm->state == BLE_FSM_ADVERTISING &&
                    !issue_advertising(fsm, now_us, true, commands)) {
                    ready_to_sleep(fsm);
                }
            } else if (fsm->state == BLE_FSM_CONNECTED &&
                       event->bool_value) {
                runtime_command_t disconnect = runtime_command_make(
                    RCMD_BLE_DISCONNECT, fsm->operation_id);
                disconnect.conn_handle = fsm->conn_handle;
                runtime_command_list_push(commands, &disconnect);
            }
            return;

        case RTEVT_BLE_ADV_REFRESH:
            if (fsm->state == BLE_FSM_ADVERTISING) {
                check_deadline(fsm, now_us);
                if (fsm->state == BLE_FSM_ADVERTISING &&
                    !issue_advertising(fsm, now_us, true, commands)) {
                    ready_to_sleep(fsm);
                }
            }
            return;

        case RTEVT_BLE_READY:
            if (fsm->state != BLE_FSM_STARTING ||
                event->operation_id != fsm->operation_id) {
                return;
            }
            check_deadline(fsm, now_us);
            if (fsm->state == BLE_FSM_STARTING) {
                fsm->state = BLE_FSM_ADVERTISING;
                if (!issue_advertising(fsm, now_us, false, commands)) {
                    ready_to_sleep(fsm);
                }
            }
            return;

        case RTEVT_BLE_START_FAILED:
            if ((fsm->state == BLE_FSM_STARTING ||
                 fsm->state == BLE_FSM_ADVERTISING) &&
                event->operation_id == fsm->operation_id) {
                ready_to_sleep(fsm);
            }
            return;

        case RTEVT_CLIENT_CONNECTED:
            if (fsm->state != BLE_FSM_ADVERTISING ||
                event->operation_id != fsm->operation_id ||
                event->request_id != fsm->adv_request_id ||
                event->conn_handle == UINT16_MAX) {
                return;
            }
            check_deadline(fsm, now_us);
            if (fsm->state == BLE_FSM_ADVERTISING) {
                fsm->state = BLE_FSM_CONNECTED;
                fsm->conn_handle = event->conn_handle;
                fsm->connection_deadline_us =
                    now_us + BLE_CONNECTION_TIMEOUT_US;
                fsm->adv_request_id = 0;
            }
            return;

        case RTEVT_CLIENT_DISCONNECTED:
            if (fsm->state == BLE_FSM_CONNECTED &&
                event->conn_handle == fsm->conn_handle) {
                ready_to_sleep(fsm);
            }
            return;

        case RTEVT_DEADLINE_CHECK:
            check_deadline(fsm, now_us);
            return;

        case RTEVT_ACTION_FAILED:
            if ((event->command_kind == RCMD_BLE_START ||
                 event->command_kind == RCMD_BLE_START_ADVERTISING ||
                 event->command_kind == RCMD_BLE_REFRESH_ADVERTISING) &&
                (fsm->state == BLE_FSM_STARTING ||
                 fsm->state == BLE_FSM_ADVERTISING) &&
                event->operation_id == fsm->operation_id) {
                ready_to_sleep(fsm);
            }
            return;

        default:
            return;
    }
}
