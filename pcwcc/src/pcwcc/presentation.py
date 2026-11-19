from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum, auto
from typing import Optional, Set, Tuple

from .core_logic.models import FanModeStatus, OutputLimitState, OutputLimitStatus

from .core import (
    DeviceInfo, OperationStatus, OP_STATE_IN_SERVICE, OP_STATE_DONE, OP_STATE_ERROR,
    OP_TYPE_FAN_CALIBRATION, OP_TYPE_SETUP_FANS,
    PROFILE_ACTION_CLEAR, PROFILE_ACTION_SAVE_CURRENT,
)


class Action(Enum):
    SCAN = auto()
    PAIR = auto()
    CONNECT = auto()
    DISCONNECT = auto()
    APPLY = auto()
    DISCARD = auto()
    CALIBRATE = auto()
    SETUP_FANS = auto()
    DELETE_PAIRED = auto()
    AUTO_CONNECT = auto()
    APPLY_PROFILE = auto()
    SAVE_PROFILE = auto()
    CLEAR_PROFILE = auto()
    EDIT_PROFILE = auto()
    SET_OUTPUT_MAX = auto()
    RESET_OUTPUT_LIMIT = auto()


class ConnState(Enum):
    DISCONNECTED = auto()
    CONNECTING = auto()
    CONNECTED = auto()
    DISCONNECTING = auto()


class ConnEventKind(Enum):
    CONNECT_REQUEST = auto()
    OLD_SESSION_CLOSED = auto()
    CONNECT_SUCCEEDED = auto()
    CONNECT_FAILED = auto()
    LINK_CLOSED = auto()
    AUTO_STOPPED = auto()


@dataclass(frozen=True)
class ConnEvent:
    kind: ConnEventKind
    connection_id: Optional[object] = None
    device: Optional[DeviceInfo] = None
    auto_run_id: Optional[object] = None
    error: Optional[str] = None


class SelectionSource(Enum):
    FOUND = auto()
    PAIRED = auto()


@dataclass(frozen=True)
class AppState:
    conn: ConnState = ConnState.DISCONNECTED
    active_action: Optional[Action] = None
    found_devices: Tuple[DeviceInfo, ...] = ()
    paired_devices: Tuple[DeviceInfo, ...] = ()
    selected_device: Optional[DeviceInfo] = None
    selected_source: Optional[SelectionSource] = None
    connected_device: Optional[DeviceInfo] = None
    auto_enabled: bool = False
    connection_id: Optional[object] = None
    auto_run_id: Optional[object] = None
    status: str = "Ready"
    error: Optional[str] = None
    command_slot: Optional[int] = None
    command_id: Optional[object] = None
    operation_status: Optional[OperationStatus] = None
    output_limit: Optional[OutputLimitStatus] = None
    fan_mode_status: Optional[FanModeStatus] = None

    @property
    def busy(self) -> bool:
        return self.active_action is not None


@dataclass(frozen=True)
class UiModel:
    enabled_actions: Set[Action]
    status_text: str
    busy: bool
    auto_enabled: bool
    paired_highlight: Optional[DeviceInfo]
    can_edit_params: bool


PROFILE_ACTIONS = {Action.APPLY_PROFILE, Action.SAVE_PROFILE, Action.CLEAR_PROFILE, Action.EDIT_PROFILE}
PROFILE_COMMANDS = {
    PROFILE_ACTION_SAVE_CURRENT: Action.SAVE_PROFILE,
    PROFILE_ACTION_CLEAR: Action.CLEAR_PROFILE,
}
OPERATION_ACTIONS = {
    OP_TYPE_FAN_CALIBRATION: Action.CALIBRATE,
    OP_TYPE_SETUP_FANS: Action.SETUP_FANS,
}

ASYNC_ACTIONS = PROFILE_ACTIONS | {
    Action.SCAN,
    Action.PAIR,
    Action.CONNECT,
    Action.DISCONNECT,
    Action.APPLY,
    Action.DISCARD,
    Action.CALIBRATE,
    Action.SETUP_FANS,
    Action.AUTO_CONNECT,
    Action.SET_OUTPUT_MAX,
    Action.RESET_OUTPUT_LIMIT,
}

NEEDS_DEVICE = {
    Action.PAIR,
    Action.CONNECT,
}


def operation_running(status: Optional[OperationStatus]) -> bool:
    return status is not None and status.state == OP_STATE_IN_SERVICE


def derive_ui(state: AppState) -> UiModel:
    if state.auto_enabled:
        enabled = {Action.AUTO_CONNECT}
    elif state.busy or state.conn in (ConnState.CONNECTING, ConnState.DISCONNECTING):
        enabled = set()
    elif state.conn == ConnState.CONNECTED and operation_running(state.operation_status):
        enabled = {Action.DISCONNECT}
    elif state.conn == ConnState.CONNECTED:
        enabled = PROFILE_ACTIONS | {
            Action.DISCONNECT,
            Action.APPLY,
            Action.DISCARD,
            Action.CALIBRATE,
            Action.SETUP_FANS,
        }
    elif state.selected_device is not None and state.selected_source == SelectionSource.PAIRED:
        enabled = {Action.SCAN, Action.CONNECT, Action.DELETE_PAIRED, Action.AUTO_CONNECT}
    elif state.selected_device is not None and state.selected_source == SelectionSource.FOUND:
        enabled = {Action.SCAN, Action.PAIR, Action.AUTO_CONNECT}
    else:
        enabled = {Action.SCAN, Action.AUTO_CONNECT}

    if Action.APPLY in enabled and state.output_limit is not None:
        enabled.add(Action.RESET_OUTPUT_LIMIT)
        if state.output_limit.state not in {
            OutputLimitState.MEASURE, OutputLimitState.VERIFY_FORMULA, OutputLimitState.VERIFY_STEP,
            OutputLimitState.SENSOR_ERROR, OutputLimitState.CONFIRMING, OutputLimitState.VERIFY_MINIMUM,
        }:
            enabled.add(Action.SET_OUTPUT_MAX)

    highlight = state.connected_device if state.conn == ConnState.CONNECTED else None
    return UiModel(
        enabled_actions=enabled,
        status_text=state.status,
        busy=state.busy,
        auto_enabled=state.auto_enabled,
        paired_highlight=highlight,
        can_edit_params=Action.APPLY in enabled,
    )


@dataclass(frozen=True)
class Command:
    action: Action
    device: Optional[DeviceInfo] = None
    connection_id: Optional[object] = None


class AppModel:
    def __init__(self, initial: Optional[AppState] = None) -> None:
        self._state = initial or AppState()

    @property
    def state(self) -> AppState:
        return self._state

    @property
    def ui(self) -> UiModel:
        return derive_ui(self._state)

    def dispatch(self, action: Action, *, slot: Optional[int] = None) -> Optional[Command]:
        ui = derive_ui(self._state)
        if action not in ui.enabled_actions:
            return None

        device: Optional[DeviceInfo] = None
        if action in NEEDS_DEVICE:
            device = self._state.selected_device
            if device is None:
                return None

        if action in PROFILE_ACTIONS and slot is None:
            return None
        connection_id = self._state.connection_id
        if action == Action.CONNECT:
            connection_id = object()
            self._state = replace(self._state, conn=ConnState.CONNECTING, connection_id=connection_id)
        elif action == Action.DISCONNECT:
            if connection_id is None:
                return None
            self._state = replace(self._state, conn=ConnState.DISCONNECTING)

        if action in ASYNC_ACTIONS:
            self._state = replace(self._state, active_action=action, error=None, command_slot=slot, command_id=object())

        return Command(action=action, device=device, connection_id=connection_id)

    def finish_action(self, action: Action) -> None:
        if self._state.active_action == action:
            self._state = replace(self._state, active_action=None, command_slot=None, command_id=None)

    def handle_params_result(self, ok: bool, command_id: object) -> Optional[Action]:
        action = self._state.active_action
        if self._state.command_id is not command_id or action not in (Action.APPLY, Action.APPLY_PROFILE):
            return None
        if ok and action == Action.APPLY and self._state.command_slot is not None:
            self._state = replace(self._state, active_action=Action.SAVE_PROFILE)
        else:
            self.finish_action(action)
        return action

    def handle_profile_result(self, action: Action, slot: int) -> bool:
        if self._state.active_action != action or self._state.command_slot != slot:
            return False
        self.finish_action(action)
        return True

    def handle_operation_start_failure(self, op_type: int) -> Optional[Action]:
        action = OPERATION_ACTIONS.get(op_type)
        if action is not None and self._state.active_action == action:
            self.finish_action(action)
            return action
        return None

    def handle_fan_mode_status(self, status: FanModeStatus, connection_id: object) -> bool:
        if self._state.conn not in (ConnState.CONNECTING, ConnState.CONNECTED) or self._state.connection_id is not connection_id:
            return False
        self._state = replace(self._state, fan_mode_status=status)
        return True

    def handle_output_limit(self, status: OutputLimitStatus, connection_id: object) -> bool:
        if self._state.conn not in (ConnState.CONNECTING, ConnState.CONNECTED) or self._state.connection_id is not connection_id:
            return False
        self._state = replace(self._state, output_limit=status)
        return True

    def handle_operation_status(self, status: OperationStatus, connection_id: object) -> Optional[Action]:
        if (self._state.conn not in (ConnState.CONNECTING, ConnState.CONNECTED)
                or self._state.connection_id is not connection_id):
            return None
        self._state = replace(self._state, operation_status=status)
        action = OPERATION_ACTIONS.get(status.op_type)
        if (action is not None and self._state.active_action == action
                and status.state in (OP_STATE_DONE, OP_STATE_ERROR)):
            self.finish_action(action)
            return action
        return None

    def set_status(self, message: str) -> None:
        self._state = replace(self._state, status=message)

    def set_error(self, message: Optional[str]) -> None:
        self._state = replace(self._state, error=message)

    def set_found_devices(self, devices: list[DeviceInfo]) -> None:
        self._state = replace(self._state, found_devices=tuple(devices))

    def clear_found_devices(self) -> None:
        self._state = replace(self._state, found_devices=())
        if self._state.selected_source == SelectionSource.FOUND:
            self._state = replace(self._state, selected_device=None, selected_source=None)

    def set_paired_devices(self, devices: list[DeviceInfo]) -> None:
        self._state = replace(self._state, paired_devices=tuple(devices))

    def set_selection(
        self, source: Optional[SelectionSource], device: Optional[DeviceInfo]
    ) -> None:
        self._state = replace(self._state, selected_source=source, selected_device=device)

    def handle_connection_event(self, event: ConnEvent) -> bool:
        state = self._state
        if event.kind == ConnEventKind.AUTO_STOPPED:
            if state.auto_run_id is not event.auto_run_id or state.auto_run_id is None:
                return False
            self._state = replace(
                state, conn=ConnState.DISCONNECTED, connection_id=None, connected_device=None,
                auto_enabled=False, auto_run_id=None, active_action=None,
                command_slot=None, command_id=None, operation_status=None, output_limit=None, fan_mode_status=None,
            )
            return True
        if event.connection_id is None:
            return False
        if event.kind == ConnEventKind.CONNECT_REQUEST:
            if (not state.auto_enabled or state.auto_run_id is not event.auto_run_id
                    or state.conn != ConnState.DISCONNECTED):
                return False
            self._state = replace(state, conn=ConnState.CONNECTING, connection_id=event.connection_id)
            return True
        if state.connection_id is not event.connection_id:
            return False

        if event.kind == ConnEventKind.OLD_SESSION_CLOSED:
            return state.conn == ConnState.CONNECTING
        if event.kind == ConnEventKind.CONNECT_SUCCEEDED:
            if state.conn != ConnState.CONNECTING or event.device is None:
                return False
            self._state = replace(state, conn=ConnState.CONNECTED, connected_device=event.device)
            self.finish_action(Action.CONNECT)
            return True

        if event.kind == ConnEventKind.CONNECT_FAILED:
            if state.conn != ConnState.CONNECTING:
                return False
        elif event.kind == ConnEventKind.LINK_CLOSED:
            if state.conn not in (ConnState.CONNECTED, ConnState.DISCONNECTING):
                return False
        else:
            return False

        self._state = replace(
            state, conn=ConnState.DISCONNECTED, connection_id=None, connected_device=None,
            error=event.error,
            command_slot=None, command_id=None, operation_status=None, output_limit=None, fan_mode_status=None,
            active_action=Action.AUTO_CONNECT if state.auto_run_id is not None else None,
        )
        return True

    def start_auto(self, run_id: object) -> bool:
        if self._state.auto_run_id is not None or Action.AUTO_CONNECT not in self.ui.enabled_actions:
            return False
        self._state = replace(
            self._state, auto_enabled=True, auto_run_id=run_id,
            active_action=Action.AUTO_CONNECT, error=None,
        )
        return True

    def stop_auto(self) -> None:
        state = self._state
        if state.auto_enabled:
            conn = ConnState.DISCONNECTING if state.connection_id is not None else ConnState.DISCONNECTED
            self._state = replace(state, auto_enabled=False, conn=conn)
