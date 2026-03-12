from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum, auto
from typing import Optional, Set, Tuple

from .core import DeviceInfo


class Action(Enum):
    SCAN = auto()
    PAIR = auto()
    CONNECT = auto()
    DISCONNECT = auto()
    APPLY = auto()
    CALIBRATE = auto()
    DETECT_FAN_CONTROL = auto()
    DELETE_PAIRED = auto()
    AUTO_CONNECT = auto()


class ConnState(Enum):
    DISCONNECTED = auto()
    CONNECTING = auto()
    CONNECTED = auto()


class SelectionSource(Enum):
    FOUND = auto()
    PAIRED = auto()


@dataclass(frozen=True)
class AppState:
    conn: ConnState = ConnState.DISCONNECTED
    busy: bool = False
    active_action: Optional[Action] = None
    found_devices: Tuple[DeviceInfo, ...] = ()
    paired_devices: Tuple[DeviceInfo, ...] = ()
    selected_device: Optional[DeviceInfo] = None
    selected_source: Optional[SelectionSource] = None
    connected_device: Optional[DeviceInfo] = None
    auto_enabled: bool = False
    status: str = "Ready"
    error: Optional[str] = None


@dataclass(frozen=True)
class UiModel:
    enabled_actions: Set[Action]
    status_text: str
    busy: bool
    auto_enabled: bool
    paired_highlight: Optional[DeviceInfo]


ASYNC_ACTIONS = {
    Action.SCAN,
    Action.PAIR,
    Action.CONNECT,
    Action.DISCONNECT,
    Action.APPLY,
    Action.CALIBRATE,
    Action.DETECT_FAN_CONTROL,
    Action.AUTO_CONNECT,
}

NEEDS_DEVICE = {
    Action.PAIR,
    Action.CONNECT,
}


def derive_ui(state: AppState) -> UiModel:
    if state.auto_enabled or state.active_action == Action.AUTO_CONNECT:
        enabled = {Action.AUTO_CONNECT}
    elif state.busy:
        enabled = set()
    elif state.conn == ConnState.CONNECTED:
        enabled = {Action.DISCONNECT, Action.APPLY, Action.CALIBRATE, Action.DETECT_FAN_CONTROL}
    elif state.selected_device is not None and state.selected_source == SelectionSource.PAIRED:
        enabled = {Action.SCAN, Action.CONNECT, Action.DELETE_PAIRED, Action.AUTO_CONNECT}
    elif state.selected_device is not None and state.selected_source == SelectionSource.FOUND:
        enabled = {Action.SCAN, Action.PAIR, Action.AUTO_CONNECT}
    else:
        enabled = {Action.SCAN, Action.AUTO_CONNECT}

    highlight = state.connected_device if state.conn == ConnState.CONNECTED else None
    return UiModel(
        enabled_actions=enabled,
        status_text=state.status,
        busy=state.busy,
        auto_enabled=state.auto_enabled,
        paired_highlight=highlight,
    )


@dataclass(frozen=True)
class Command:
    action: Action
    device: Optional[DeviceInfo] = None


class AppModel:
    def __init__(self, initial: Optional[AppState] = None) -> None:
        self._state = initial or AppState()

    @property
    def state(self) -> AppState:
        return self._state

    @property
    def ui(self) -> UiModel:
        return derive_ui(self._state)

    def dispatch(self, action: Action) -> Optional[Command]:
        ui = derive_ui(self._state)
        if action not in ui.enabled_actions:
            return None

        device: Optional[DeviceInfo] = None
        if action in NEEDS_DEVICE:
            device = self._state.selected_device
            if device is None:
                return None

        if action in ASYNC_ACTIONS:
            self._state = replace(self._state, busy=True, active_action=action, error=None)

        return Command(action=action, device=device)

    def finish_action(self, action: Action) -> None:
        if self._state.active_action == action:
            self._state = replace(self._state, busy=False, active_action=None)

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

    def set_connected(self, connected: bool, device: Optional[DeviceInfo]) -> None:
        conn = ConnState.CONNECTED if connected else ConnState.DISCONNECTED
        self._state = replace(self._state, conn=conn, connected_device=device)

    def start_connecting(self) -> None:
        self._state = replace(self._state, conn=ConnState.CONNECTING)

    def set_auto_enabled(self, enabled: bool) -> bool:
        if enabled:
            ui = derive_ui(self._state)
            if Action.AUTO_CONNECT not in ui.enabled_actions:
                return False
            self._state = replace(
                self._state,
                auto_enabled=True,
                busy=True,
                active_action=Action.AUTO_CONNECT,
                error=None,
            )
            return True

        if self._state.auto_enabled:
            self._state = replace(
                self._state,
                auto_enabled=False,
                busy=False if self._state.active_action == Action.AUTO_CONNECT else self._state.busy,
                active_action=None if self._state.active_action == Action.AUTO_CONNECT else self._state.active_action,
            )
            return True
        return False
