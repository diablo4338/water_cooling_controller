from pcwcc.core import DeviceInfo, OperationStatus, OP_STATE_IN_SERVICE, OP_TYPE_SETUP_FANS
from pcwcc.presentation import Action, AppModel, AppState, ConnEvent, ConnEventKind, ConnState, SelectionSource, derive_ui


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
    assert ui.enabled_actions == {
        Action.DISCONNECT,
        Action.APPLY,
        Action.DISCARD,
        Action.CALIBRATE,
        Action.SETUP_FANS,
        Action.APPLY_PROFILE,
        Action.SAVE_PROFILE,
        Action.CLEAR_PROFILE,
        Action.EDIT_PROFILE,
    }
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
    state = AppState(active_action=Action.SCAN)
    ui = derive_ui(state)
    assert ui.enabled_actions == set()

    state = AppState(active_action=Action.AUTO_CONNECT)
    ui = derive_ui(state)
    assert ui.enabled_actions == set()


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


def test_auto_toggle_waits_for_worker_cleanup() -> None:
    model = AppModel()
    run = object()
    assert model.start_auto(run)
    assert model.state.auto_enabled
    assert model.state.busy
    assert model.state.active_action == Action.AUTO_CONNECT

    connection_id = object()
    assert model.handle_connection_event(ConnEvent(ConnEventKind.CONNECT_REQUEST, connection_id, auto_run_id=run))
    model.stop_auto()
    assert model.state.conn == ConnState.DISCONNECTING
    assert not model.state.auto_enabled
    assert model.ui.enabled_actions == set()

    device = DeviceInfo(name="Dev", address="AA:BB")
    assert not model.handle_connection_event(ConnEvent(ConnEventKind.CONNECT_SUCCEEDED, connection_id, device))
    assert model.handle_connection_event(ConnEvent(ConnEventKind.LINK_CLOSED, connection_id))
    assert model.state.busy
    assert model.handle_connection_event(ConnEvent(ConnEventKind.AUTO_STOPPED, auto_run_id=run))
    assert not model.state.busy
    assert model.state.active_action is None


def test_connection_events_are_scoped_to_the_attempt() -> None:
    model = AppModel()
    device = DeviceInfo(name="Dev", address="AA:BB")
    model.set_selection(SelectionSource.PAIRED, device)
    first = model.dispatch(Action.CONNECT)
    assert first is not None
    assert model.handle_connection_event(ConnEvent(ConnEventKind.OLD_SESSION_CLOSED, first.connection_id))
    assert model.state.conn == ConnState.CONNECTING
    assert model.state.active_action == Action.CONNECT
    assert model.ui.enabled_actions == set()
    assert model.handle_connection_event(ConnEvent(ConnEventKind.CONNECT_FAILED, first.connection_id))
    assert model.state.conn == ConnState.DISCONNECTED

    second = model.dispatch(Action.CONNECT)
    assert second is not None
    assert not model.handle_connection_event(ConnEvent(ConnEventKind.CONNECT_SUCCEEDED, first.connection_id, device))
    assert model.handle_connection_event(ConnEvent(ConnEventKind.CONNECT_SUCCEEDED, second.connection_id, device))
    assert not model.handle_connection_event(ConnEvent(ConnEventKind.LINK_CLOSED, first.connection_id))
    assert model.state.conn == ConnState.CONNECTED
    assert model.dispatch(Action.DISCONNECT) is not None
    assert model.state.conn == ConnState.DISCONNECTING
    assert model.handle_connection_event(ConnEvent(ConnEventKind.LINK_CLOSED, second.connection_id))
    assert model.state.conn == ConnState.DISCONNECTED
    assert not model.state.busy


def test_device_operation_and_local_command_have_independent_lifecycles() -> None:
    model = AppModel()
    device = DeviceInfo(name="Fan", address="AA:BB")
    model.set_selection(SelectionSource.PAIRED, device)
    command = model.dispatch(Action.CONNECT)
    assert command is not None
    model.handle_connection_event(ConnEvent(ConnEventKind.CONNECT_SUCCEEDED, command.connection_id, device))
    assert model.dispatch(Action.SETUP_FANS) is not None

    running = OperationStatus(OP_TYPE_SETUP_FANS, OP_STATE_IN_SERVICE, "")
    model.handle_operation_status(running, command.connection_id)
    assert model.handle_operation_start_failure(OP_TYPE_SETUP_FANS) == Action.SETUP_FANS
    assert model.state.active_action is None
    assert model.state.operation_status == running
    assert model.ui.enabled_actions == {Action.DISCONNECT}

    assert model.handle_connection_event(ConnEvent(ConnEventKind.LINK_CLOSED, command.connection_id))
    assert model.state.operation_status is None
    model.handle_operation_status(running, command.connection_id)
    assert model.state.operation_status is None


def test_apply_and_profile_save_share_one_busy_interval() -> None:
    device = DeviceInfo(name="Fan", address="AA:BB")
    model = AppModel(AppState(conn=ConnState.CONNECTED, connected_device=device))
    assert model.dispatch(Action.APPLY, slot=2) is not None
    command_id = model.state.command_id
    assert model.handle_params_result(True, command_id) == Action.APPLY
    assert model.state.active_action == Action.SAVE_PROFILE
    assert model.state.command_slot == 2
    assert not model.ui.can_edit_params
    assert model.ui.enabled_actions == set()
    assert model.handle_params_result(True, command_id) is None
    assert not model.handle_profile_result(Action.SAVE_PROFILE, 1)
    assert model.handle_profile_result(Action.SAVE_PROFILE, 2)
    assert model.ui.can_edit_params
    assert model.state.command_slot is None


def test_late_params_result_cannot_finish_a_retried_command() -> None:
    model = AppModel(AppState(conn=ConnState.CONNECTED))
    assert model.dispatch(Action.APPLY, slot=1)
    expired = model.state.command_id
    model.finish_action(Action.APPLY)
    assert model.dispatch(Action.APPLY, slot=2)
    current = model.state.command_id
    assert model.handle_params_result(True, expired) is None
    assert model.state.active_action == Action.APPLY
    assert model.handle_params_result(True, current) == Action.APPLY
    assert model.state.active_action == Action.SAVE_PROFILE
    assert model.state.command_slot == 2
