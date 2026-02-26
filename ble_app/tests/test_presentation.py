from ble_app.core import DeviceInfo
from ble_app.presentation import Action, AppModel, AppState, ConnState, SelectionSource, derive_ui


def test_derive_ui_basics() -> None:
    state = AppState()
    ui = derive_ui(state)
    assert Action.SCAN in ui.enabled_actions
    assert Action.SHOW_PAIRED in ui.enabled_actions
    assert Action.AUTO_CONNECT in ui.enabled_actions
    assert Action.CONNECT not in ui.enabled_actions
    assert Action.PAIR not in ui.enabled_actions
    assert Action.DISCONNECT not in ui.enabled_actions
    assert Action.DELETE_PAIRED not in ui.enabled_actions

    device = DeviceInfo(name="Dev", address="AA:BB")
    state = AppState(selected_device=device, selected_source=SelectionSource.FOUND)
    ui = derive_ui(state)
    assert Action.CONNECT in ui.enabled_actions
    assert Action.PAIR in ui.enabled_actions

    state = AppState(conn=ConnState.CONNECTED)
    ui = derive_ui(state)
    assert Action.DISCONNECT in ui.enabled_actions

    state = AppState(selected_device=device, selected_source=SelectionSource.PAIRED)
    ui = derive_ui(state)
    assert Action.DELETE_PAIRED in ui.enabled_actions


def test_derive_ui_busy_gates_actions() -> None:
    state = AppState(busy=True, active_action=Action.SCAN)
    ui = derive_ui(state)
    assert ui.enabled_actions == set()

    state = AppState(busy=True, active_action=Action.AUTO_CONNECT)
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
