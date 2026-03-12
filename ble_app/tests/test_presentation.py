from ble_app.core import DeviceInfo
from ble_app.presentation import Action, AppModel, AppState, ConnState, SelectionSource, derive_ui


def test_derive_ui_basics() -> None:
    state = AppState()
    ui = derive_ui(state)
    assert Action.SCAN in ui.enabled_actions
    assert Action.AUTO_CONNECT in ui.enabled_actions
    assert Action.CONNECT not in ui.enabled_actions
    assert Action.PAIR not in ui.enabled_actions
    assert Action.DISCONNECT not in ui.enabled_actions
    assert Action.DELETE_PAIRED not in ui.enabled_actions
    assert ui.paired_highlight is None

    device = DeviceInfo(name="Dev", address="AA:BB")
    state = AppState(selected_device=device, selected_source=SelectionSource.FOUND)
    ui = derive_ui(state)
    assert ui.enabled_actions == {Action.SCAN, Action.PAIR, Action.AUTO_CONNECT}
    assert ui.paired_highlight is None

    state = AppState(conn=ConnState.CONNECTED)
    ui = derive_ui(state)
    assert ui.enabled_actions == {Action.DISCONNECT, Action.APPLY}
    assert ui.paired_highlight is None

    state = AppState(selected_device=device, selected_source=SelectionSource.PAIRED)
    ui = derive_ui(state)
    assert ui.enabled_actions == {Action.SCAN, Action.CONNECT, Action.DELETE_PAIRED, Action.AUTO_CONNECT}
    assert ui.paired_highlight is None


def test_derive_ui_paired_highlight_connected_only() -> None:
    device = DeviceInfo(name="Dev", address="AA:BB")
    state = AppState(conn=ConnState.CONNECTED, connected_device=device)
    ui = derive_ui(state)
    assert ui.paired_highlight == device

    state = AppState(conn=ConnState.CONNECTING, connected_device=device)
    ui = derive_ui(state)
    assert ui.paired_highlight is None

    state = AppState(conn=ConnState.DISCONNECTED, connected_device=device)
    ui = derive_ui(state)
    assert ui.paired_highlight is None


def test_derive_ui_busy_gates_actions() -> None:
    state = AppState(busy=True, active_action=Action.SCAN)
    ui = derive_ui(state)
    assert ui.enabled_actions == set()

    state = AppState(busy=True, active_action=Action.AUTO_CONNECT)
    ui = derive_ui(state)
    assert ui.enabled_actions == {Action.AUTO_CONNECT}


def test_derive_ui_auto_connect_locks_all_actions() -> None:
    state = AppState(auto_enabled=True)
    ui = derive_ui(state)
    assert ui.enabled_actions == {Action.AUTO_CONNECT}


def test_dispatch_gates_and_sets_busy() -> None:
    model = AppModel()
    assert model.dispatch(Action.PAIR) is None
    assert model.state.busy is False

    device = DeviceInfo(name="Dev", address="AA:BB")
    model.set_selection(SelectionSource.FOUND, device)
    cmd = model.dispatch(Action.PAIR)
    assert cmd is not None
    assert cmd.action == Action.PAIR
    assert cmd.device == device
    assert model.state.busy is True
    assert model.state.active_action == Action.PAIR

    model.finish_action(Action.PAIR)
    assert model.state.busy is False


def test_auto_toggle_sets_state() -> None:
    model = AppModel()
    assert model.set_auto_enabled(True) is True
    assert model.state.auto_enabled is True
    assert model.state.busy is True
    assert model.state.active_action == Action.AUTO_CONNECT

    assert model.set_auto_enabled(False) is True
    assert model.state.auto_enabled is False
    assert model.state.busy is False
    assert model.state.active_action is None
