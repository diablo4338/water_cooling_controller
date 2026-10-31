#include "ble_fsm.h"

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
    return !fsm || fsm->state != BLE_FSM_OFF ||
           fsm->conn_handle != UINT16_MAX;
}

static void begin_start(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                        int64_t now_us, runtime_command_list_t *commands) {
    fsm->state = BLE_FSM_STARTING;
    fsm->window_deadline_us = now_us + config->window_us;
    fsm->operation_id = runtime_event_next_id();
    fsm->pending_restart = false;
    fsm->conn_handle = UINT16_MAX;
    fsm->stop_retries = 0;
    fsm->adv_request_id = 0;
    fsm->adv_deadline_us = 0;

    runtime_command_t command = runtime_command_make(
        RCMD_BLE_START, fsm->operation_id);
    command.duration_ms = (uint32_t)(config->window_us / 1000);
    runtime_command_list_push(commands, &command);
}

static void begin_stop(ble_fsm_t *fsm, runtime_command_list_t *commands) {
    fsm->state = BLE_FSM_STOPPING;
    fsm->stop_operation_id = runtime_event_next_id();
    fsm->conn_handle = UINT16_MAX;
    fsm->stop_retries = 0;
    fsm->adv_request_id = 0;
    fsm->adv_deadline_us = 0;

    runtime_command_t command = runtime_command_make(
        RCMD_BLE_STOP, fsm->stop_operation_id);
    runtime_command_list_push(commands, &command);
}

static void finish_stop(ble_fsm_t *fsm, const ble_fsm_config_t *config,
                        int64_t now_us, runtime_command_list_t *commands) {
    if (fsm->pending_restart) {
        begin_start(fsm, config, now_us, commands);
    } else {
        fsm->state = BLE_FSM_OFF;
        fsm->window_deadline_us = 0;
        fsm->conn_handle = UINT16_MAX;
        fsm->stop_operation_id = 0;
        fsm->stop_retries = 0;
        fsm->adv_request_id = 0;
        fsm->adv_deadline_us = 0;
    }
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
    fsm->adv_deadline_us = fsm->window_deadline_us;

    runtime_command_t command = runtime_command_make(
        refresh ? RCMD_BLE_REFRESH_ADVERTISING : RCMD_BLE_START_ADVERTISING,
        fsm->operation_id);
    command.request_id = fsm->adv_request_id;
    command.duration_ms = (uint32_t)remaining;
    runtime_command_list_push(commands, &command);
    return true;
}

void ble_fsm_step(ble_fsm_t *fsm, const runtime_event_t *event,
                  const ble_fsm_config_t *config, int64_t now_us,
                  runtime_command_list_t *commands) {
    if (!fsm || !event || !config || !commands) return;

    switch (event->kind) {
        case RTEVT_EVENT_BUS_FAULT:
            if (fsm->state != BLE_FSM_OFF) {
                fsm->state = BLE_FSM_ERROR;
            }
            return;

        case RTEVT_THERMAL_BUTTON_PRESSED:
        case RTEVT_BLE_WINDOW_REQUEST:
            switch (fsm->state) {
                case BLE_FSM_OFF:
                case BLE_FSM_ERROR:
                    begin_start(fsm, config, now_us, commands);
                    break;
                case BLE_FSM_STARTING:
                case BLE_FSM_ADVERTISING:
                    extend_window(fsm, config, now_us);
                    break;
                case BLE_FSM_CONNECTED:
                    break;
                case BLE_FSM_STOPPING:
                    fsm->pending_restart = true;
                    break;
                default:
                    break;
            }
            return;

        case RTEVT_BLE_PAIRING_REQUEST:
            switch (fsm->state) {
                case BLE_FSM_OFF:
                case BLE_FSM_ERROR:
                    begin_start(fsm, config, now_us, commands);
                    break;
                case BLE_FSM_STARTING:
                    extend_window(fsm, config, now_us);
                    break;
                case BLE_FSM_ADVERTISING:
                    extend_window(fsm, config, now_us);
                    (void)issue_advertising(fsm, now_us, true, commands);
                    break;
                case BLE_FSM_CONNECTED:
                    extend_window(fsm, config, now_us);
                    fsm->pending_restart = true;
                    if (fsm->conn_handle != UINT16_MAX) {
                        runtime_command_t disconnect = runtime_command_make(
                            RCMD_BLE_DISCONNECT, fsm->operation_id);
                        disconnect.conn_handle = fsm->conn_handle;
                        runtime_command_list_push(commands, &disconnect);
                    }
                    break;
                case BLE_FSM_STOPPING:
                    fsm->pending_restart = true;
                    break;
                default:
                    break;
            }
            return;

        case RTEVT_BLE_PAIR_TIMEOUT:
            /* Pairing mode was deactivated by the dispatcher.  Switch a live
             * advertisement to the main service, and terminate an unpaired
             * client.  An authenticated client is left alone. */
            if (fsm->state == BLE_FSM_ADVERTISING) {
                if (!issue_advertising(fsm, now_us, true, commands)) {
                    begin_stop(fsm, commands);
                }
            } else if (fsm->state == BLE_FSM_CONNECTED && event->bool_value) {
                begin_stop(fsm, commands);
            }
            return;

        case RTEVT_BLE_ADV_REFRESH:
            if (fsm->state == BLE_FSM_ADVERTISING) {
                if (!issue_advertising(fsm, now_us, true, commands)) {
                    begin_stop(fsm, commands);
                }
            }
            return;

        case RTEVT_BLE_READY:
            if (fsm->state != BLE_FSM_STARTING ||
                event->operation_id != fsm->operation_id) {
                return;
            }
            if (ble_fsm_remaining_ms(fsm, now_us) > 0) {
                fsm->state = BLE_FSM_ADVERTISING;
                if (!issue_advertising(fsm, now_us, false, commands)) {
                    begin_stop(fsm, commands);
                }
            } else {
                begin_stop(fsm, commands);
            }
            return;

        case RTEVT_BLE_START_FAILED:
            if ((fsm->state == BLE_FSM_STARTING ||
                 fsm->state == BLE_FSM_ADVERTISING) &&
                event->operation_id == fsm->operation_id) {
                begin_stop(fsm, commands);
            }
            return;

        case RTEVT_CLIENT_CONNECTED:
            if (fsm->state == BLE_FSM_ADVERTISING &&
                (fsm->adv_request_id == 0 ||
                 event->request_id == fsm->adv_request_id)) {
                fsm->state = BLE_FSM_CONNECTED;
                fsm->conn_handle = event->conn_handle;
                fsm->pending_restart = false;
            }
            return;

        case RTEVT_CLIENT_DISCONNECTED:
            if (fsm->state == BLE_FSM_CONNECTED &&
                (event->conn_handle == UINT16_MAX ||
                 event->conn_handle == fsm->conn_handle)) {
                /* Always stop; never resume advertising on disconnect. */
                begin_stop(fsm, commands);
            }
            return;

        case RTEVT_ADV_COMPLETE:
            if (fsm->state == BLE_FSM_ADVERTISING &&
                (fsm->adv_request_id == 0 ||
                 event->request_id == fsm->adv_request_id)) {
                bool extended = fsm->window_deadline_us > fsm->adv_deadline_us;
                if (extended && issue_advertising(fsm, now_us, false, commands)) {
                    return;
                }
                begin_stop(fsm, commands);
            }
            return;

        case RTEVT_DEADLINE_CHECK:
            if ((fsm->state == BLE_FSM_STARTING ||
                 fsm->state == BLE_FSM_ADVERTISING) &&
                fsm->window_deadline_us > 0 &&
                now_us >= fsm->window_deadline_us) {
                begin_stop(fsm, commands);
            }
            return;

        case RTEVT_BLE_STOPPED:
            if (fsm->state == BLE_FSM_STOPPING &&
                event->operation_id == fsm->stop_operation_id) {
                finish_stop(fsm, config, now_us, commands);
            }
            return;

        case RTEVT_BLE_STOP_FAILED:
            if (fsm->state == BLE_FSM_STOPPING &&
                event->operation_id == fsm->stop_operation_id) {
                if (fsm->stop_retries < config->max_stop_retries) {
                    ++fsm->stop_retries;
                    runtime_command_t command = runtime_command_make(
                        RCMD_BLE_STOP, fsm->stop_operation_id);
                    runtime_command_list_push(commands, &command);
                } else {
                    fsm->state = BLE_FSM_ERROR;
                }
            }
            return;

        case RTEVT_ACTION_FAILED:
            if ((event->command_kind == RCMD_BLE_START ||
                 event->command_kind == RCMD_BLE_START_ADVERTISING ||
                 event->command_kind == RCMD_BLE_REFRESH_ADVERTISING) &&
                (fsm->state == BLE_FSM_STARTING ||
                 fsm->state == BLE_FSM_ADVERTISING) &&
                event->operation_id == fsm->operation_id) {
                begin_stop(fsm, commands);
            } else if (event->command_kind == RCMD_BLE_STOP &&
                       fsm->state == BLE_FSM_STOPPING &&
                       event->operation_id == fsm->stop_operation_id) {
                if (fsm->stop_retries < config->max_stop_retries) {
                    ++fsm->stop_retries;
                    runtime_command_t command = runtime_command_make(
                        RCMD_BLE_STOP, fsm->stop_operation_id);
                    runtime_command_list_push(commands, &command);
                } else {
                    fsm->state = BLE_FSM_ERROR;
                }
            } else if (event->command_kind == RCMD_BLE_STOP_ADVERTISING &&
                       fsm->state == BLE_FSM_ADVERTISING &&
                       event->operation_id == fsm->operation_id) {
                begin_stop(fsm, commands);
            } else if (event->command_kind == RCMD_BLE_DISCONNECT &&
                       fsm->state == BLE_FSM_CONNECTED &&
                       event->operation_id == fsm->operation_id) {
                begin_stop(fsm, commands);
            }
            return;

        default:
            return;
    }
}
