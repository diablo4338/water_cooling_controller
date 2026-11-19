import asyncio
import struct
from dataclasses import replace
from types import SimpleNamespace
from unittest.mock import AsyncMock
from datetime import datetime

import pytest
from PySide6.QtWidgets import QApplication, QHBoxLayout, QVBoxLayout

from pcwcc.core import (
    BleAppCore,
    DeviceInfo,
    DeviceParams,
    MetricsSnapshot,
    ParamsStatus,
    PROFILE_ACTION_APPLY,
    PROFILE_ACTION_CLEAR,
    PROFILE_ACTION_SAVE_CURRENT,
    UUID_CONFIG_PROFILE_METADATA,
    UUID_CONFIG_PROFILES,
    UUID_FAN2_SPEED_VALUE,
    UUID_FAN3_SPEED_VALUE,
    UUID_FAN4_SPEED_VALUE,
    UUID_FAN_SPEED_VALUE,
    UUID_CURRENT_VALUE,
    UUID_TEMP0_VALUE,
    UUID_TEMP1_VALUE,
    UUID_TEMP2_VALUE,
    UUID_TEMP3_VALUE,
    UUID_VOLTAGE_VALUE,
    decode_profile_slots,
    decode_profile_metadata,
    encode_profile_command,
    encode_profile_metadata,
)
from pcwcc import gui as gui_module
from pcwcc.core_logic.models import (
    DEFAULT_PARAMS,
    OutputLimitState,
    OutputLimitStatus,
    ProfileApplyResult,
    ProfileSkipReason,
)
from pcwcc.core_logic.protocol import PARAM_FIELD_FAN_MIN_SPEED, PARAM_STATUS_INVALID
from pcwcc.gui_logic import window_updates as window_updates_module
from pcwcc.presentation import Action, AppModel, ConnEvent, ConnEventKind, ConnState, SelectionSource


def _connected_event(model, device: DeviceInfo) -> ConnEvent:
    model.set_selection(SelectionSource.PAIRED, device)
    command = model.dispatch(Action.CONNECT)
    assert command is not None
    return ConnEvent(ConnEventKind.CONNECT_SUCCEEDED, command.connection_id, device)


class _FakeClient:
    def __init__(self, payloads: dict[str, bytes]) -> None:
        self._payloads = payloads

    async def read_gatt_char(self, uuid: str) -> bytes:
        return self._payloads[uuid]

    async def write_gatt_char(self, uuid: str, data: bytes, response: bool) -> None:
        self._write = (uuid, data, response)


@pytest.mark.asyncio
async def test_profile_protocol_reads_slots_and_sends_device_apply_command(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    core = BleAppCore()
    client = _FakeClient({UUID_CONFIG_PROFILES: b"\x01\x05"})
    core.client = client

    assert await core.read_profile_slots(timeout=1.0) == frozenset({1, 3})
    await core.apply_profile(3, timeout=1.0)

    assert client._write == (
        UUID_CONFIG_PROFILES,
        encode_profile_command(PROFILE_ACTION_APPLY, 3),
        True,
    )
    assert encode_profile_command(PROFILE_ACTION_SAVE_CURRENT, 2) == b"\x01\x02"
    assert encode_profile_command(PROFILE_ACTION_CLEAR, 3) == b"\x03\x03"
    assert decode_profile_slots(b"\x01\x07") == frozenset({1, 2, 3})


def test_profile_protocol_rejects_bad_slots_and_masks() -> None:
    with pytest.raises(ValueError):
        encode_profile_command(PROFILE_ACTION_APPLY, 0)
    with pytest.raises(ValueError):
        encode_profile_command(PROFILE_ACTION_APPLY, 4)
    with pytest.raises(RuntimeError):
        decode_profile_slots(b"\x01\x20")


def test_profile_metadata_codec_preserves_unicode_and_enforces_limits() -> None:
    payload = encode_profile_metadata("Тихий", "Низкий шум")

    assert decode_profile_metadata(payload) == {
        "title": "Тихий",
        "description": "Низкий шум",
    }
    assert decode_profile_metadata(b"\x01\x00\x00") == {
        "title": "",
        "description": "",
    }
    with pytest.raises(ValueError):
        encode_profile_metadata("Я" * 20, "")


@pytest.mark.asyncio
async def test_profile_metadata_uses_per_slot_device_characteristic(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    core = BleAppCore()
    payload = encode_profile_metadata("Quiet", "Low noise")
    client = _FakeClient({UUID_CONFIG_PROFILE_METADATA[1]: payload})
    core.client = client

    assert await core.read_profile_metadata(2, timeout=1.0) == {
        "title": "Quiet",
        "description": "Low noise",
    }
    await core.save_profile_metadata(2, "Silent", "Very low noise", timeout=1.0)

    assert client._write == (
        UUID_CONFIG_PROFILE_METADATA[1],
        encode_profile_metadata("Silent", "Very low noise"),
        True,
    )


@pytest.mark.asyncio
async def test_read_metrics_snapshot_reads_temperatures_and_fans(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    core = BleAppCore()
    core.client = _FakeClient(
        {
            UUID_TEMP0_VALUE: struct.pack("<f", 11.5),
            UUID_TEMP1_VALUE: struct.pack("<f", 22.5),
            UUID_TEMP2_VALUE: struct.pack("<f", 33.5),
            UUID_TEMP3_VALUE: struct.pack("<f", 44.5),
            UUID_FAN_SPEED_VALUE: struct.pack("<f", 1000.0),
            UUID_FAN2_SPEED_VALUE: struct.pack("<f", 1100.0),
            UUID_FAN3_SPEED_VALUE: struct.pack("<f", 1200.0),
            UUID_FAN4_SPEED_VALUE: struct.pack("<f", 1300.0),
            UUID_VOLTAGE_VALUE: struct.pack("<f", 12.25),
            UUID_CURRENT_VALUE: struct.pack("<f", 275.5),
        }
    )

    snapshot = await core.read_metrics_snapshot(timeout=1.0, retries=0)

    assert snapshot == MetricsSnapshot(
        temperatures=(11.5, 22.5, 33.5, 44.5),
        fan_speeds=(1000.0, 1100.0, 1200.0, 1300.0),
        voltage_v=12.25,
        current_ma=275.5,
    )


def test_temp_indicator_uses_device_params_snapshot_while_form_is_dirty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    monkeypatch.setattr(gui_module, "save_device_params", lambda *args, **kwargs: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)

    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    device = DeviceInfo(name="Test", address="AA:BB")
    params = DeviceParams(
        fan_min_speed=10,
        fan_control_type=0,
        fan_max_temp=45,
        fan_off_delta=2,
        fan_start_temp=35,
        fan_mode=0,
        fan_monitoring_enabled=True,
        fan2_monitoring_enabled=True,
        fan3_monitoring_enabled=True,
        fan4_monitoring_enabled=True,
        overheat_alarm_enabled=True,
    )
    window.on_connection_event(_connected_event(window.model, device))
    window.on_params_received(params)
    window.on_metrics_received(
        MetricsSnapshot(
            temperatures=(44.0, None, None, None),
            fan_speeds=(None, None, None, None),
            voltage_v=12.0,
            current_ma=100.0,
        )
    )

    assert "#eab308" in window.temp_indicators[0].styleSheet()
    assert window.voltage_field.text() == "12.00"
    assert window.current_field.text() == "100.0"

    fan_max_temp = next(
        item["widget"] for item in window.param_fields if item["spec"]["key"] == "fan_max_temp"
    )
    fan_max_temp.setValue(30)

    assert "#eab308" in window.temp_indicators[0].styleSheet()

    window.close()
    app.processEvents()


def test_param_field_change_does_not_submit_write(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    monkeypatch.setattr(gui_module, "save_device_params", lambda *args, **kwargs: None)

    submitted = []

    def _submit_record(self, coro):
        submitted.append(coro)
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_record)

    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    device = DeviceInfo(name="Test", address="AA:BB")
    params = DeviceParams(
        fan_min_speed=10,
        fan_control_type=0,
        fan_max_temp=45,
        fan_off_delta=2,
        fan_start_temp=35,
        fan_mode=0,
        fan_monitoring_enabled=True,
        fan2_monitoring_enabled=True,
        fan3_monitoring_enabled=True,
        fan4_monitoring_enabled=True,
        overheat_alarm_enabled=True,
    )
    window.on_connection_event(_connected_event(window.model, device))
    window.on_params_received(params)
    submitted.clear()

    fan_min_speed = next(
        item["widget"] for item in window.param_fields if item["spec"]["key"] == "fan_min_speed"
    )
    fan_min_speed.setValue(11)

    assert submitted == []

    window.close()
    app.processEvents()


def test_params_are_editable_immediately_after_connection(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    monkeypatch.setattr(window_updates_module, "load_device_params", lambda _address: DEFAULT_PARAMS)
    monkeypatch.setattr(gui_module.BleWorker, "submit", lambda _self, coro: coro.close())

    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    device = DeviceInfo(name="Test", address="AA:BB")

    window.on_connection_event(_connected_event(window.model, device))

    assert all(item["widget"].isEnabled() for item in window.param_fields)
    assert window._device_params_snapshot == DEFAULT_PARAMS

    window.close()
    app.processEvents()


def test_profile_button_applies_saved_profile_in_one_click(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    monkeypatch.setattr(gui_module, "save_device_params", lambda *args, **kwargs: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    applied_profiles = []
    dispatched_actions = []
    monkeypatch.setattr(window.worker, "apply_profile", lambda index, command_id: applied_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    monkeypatch.setattr(
        window, "_dispatch_action", lambda action: dispatched_actions.append(action)
    )
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window._device_profile_slots = {1}
    window._set_params_fields(DEFAULT_PARAMS, save=False)
    window._refresh_profile_buttons()
    window._apply_ui()

    window.profile_buttons[0].click()

    assert window.profile_buttons[0].isChecked()
    assert window._current_params() == DEFAULT_PARAMS
    assert applied_profiles == [1]
    assert not dispatched_actions

    window.close()
    app.processEvents()


def test_apply_saves_settings_to_selected_empty_profile(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    monkeypatch.setattr(gui_module, "save_device_params", lambda *args, **kwargs: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    saved_profiles = []
    dispatched_actions = []
    def dispatch(action):
        dispatched_actions.append(action)
        window.model.dispatch(action, slot=window._selected_profile_index)

    monkeypatch.setattr(window, "_dispatch_action", dispatch)
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window._set_params_fields(DEFAULT_PARAMS, save=False)
    window._refresh_profile_buttons()
    window._apply_ui()

    window.profile_buttons[1].click()
    assert not dispatched_actions
    fan_min_speed = next(
        item["widget"]
        for item in window.param_fields
        if item["spec"]["key"] == "fan_min_speed"
    )
    fan_min_speed.setValue(35)
    window.on_apply()

    assert window.model.state.command_slot == 2
    monkeypatch.setattr(window.worker, "save_profile", lambda index: saved_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    window.on_params_command_done(
        window.model.state.command_id, ParamsStatus(ok=True, status=0, field_id=None), ""
    )
    assert saved_profiles == [2]
    assert dispatched_actions == [Action.APPLY]
    assert window.model.state.active_action == Action.SAVE_PROFILE
    assert not window.apply_button.isEnabled()
    assert not window.profile_buttons[1].isEnabled()

    window.close()
    app.processEvents()


def test_rejected_apply_does_not_leave_profile_save_pending(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    window._selected_profile_index = 2
    monkeypatch.setattr(window.model, "dispatch", lambda action, **kwargs: None)

    window.on_apply()

    assert window.model.state.command_slot is None

    window.close()
    app.processEvents()


def test_profile_can_be_deselected_to_apply_without_saving_to_profile(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    saved_profiles = []
    dispatched_actions = []
    applied_profiles = []
    monkeypatch.setattr(window.worker, "apply_profile", lambda index, command_id: applied_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    monkeypatch.setattr(
        window, "_dispatch_action", lambda action: dispatched_actions.append(action)
    )
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window._device_profile_slots = {1}
    window._set_params_fields(DEFAULT_PARAMS, save=False)
    window._refresh_profile_buttons()
    window._apply_ui()

    window.profile_buttons[0].click()
    assert applied_profiles == [1]
    window.on_params_command_done(
        window.model.state.command_id, ParamsStatus(ok=True, status=0, field_id=None), ""
    )
    window.profile_buttons[0].click()
    window.on_apply()

    assert window._selected_profile_index is None
    assert not window.profile_buttons[0].isChecked()
    assert not saved_profiles
    assert dispatched_actions == [Action.APPLY]

    window.close()
    app.processEvents()


def test_profile_metadata_is_saved_on_device_and_preserved_when_cleared(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    saved_metadata = []
    monkeypatch.setattr(
        window.worker,
        "save_profile_metadata",
        lambda index, title, description: saved_metadata.append(
            (index, title, description)
        ),
    )
    cleared_profiles = []
    monkeypatch.setattr(window.worker, "clear_profile", lambda index: cleared_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window._device_profile_slots = {1}
    window._refresh_profile_buttons()
    assert window.profile_status_bars[0].styleSheet() == "background:#22c55e;"
    assert window.profile_status_bars[1].styleSheet() == "background:#9ca3af;"
    assert window.profile_buttons[0].size().width() == 75
    assert window.profile_buttons[0].size().height() == 31

    window._save_profile_metadata(1, "Quiet", "Low noise setup")
    assert saved_metadata == [(1, "Quiet", "Low noise setup")]
    assert window.profile_buttons[0].text() == "P1"
    window.on_profile_metadata_done(
        1, {"title": "Quiet", "description": "Low noise setup"}, True, ""
    )
    assert window.profile_buttons[0].text() == "Quiet"
    assert window.profile_buttons[0].toolTip() == "Low noise setup"

    window._selected_profile_index = 1
    window._apply_ui()
    window.on_clear_profile()
    assert cleared_profiles == [1]
    assert 1 in window._device_profile_slots
    window.on_profiles_received(set())
    window.on_profile_action_done(3, 1, True, "")
    assert 1 not in window._device_profile_slots
    assert window._selected_profile_index is None
    assert window.profile_buttons[0].text() == "Quiet"
    assert window.profile_status_bars[0].styleSheet() == "background:#9ca3af;"

    window.close()
    app.processEvents()


def test_debug_panels_are_rendered_in_right_column(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)

    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow(debug=True)

    container = window.centralWidget()
    root_layout = container.layout()
    assert isinstance(root_layout, QVBoxLayout)

    body_layout = root_layout.itemAt(0).layout()
    assert isinstance(body_layout, QHBoxLayout)
    assert body_layout.count() == 2

    debug_panel = body_layout.itemAt(1).widget()
    assert debug_panel is not None

    debug_layout = debug_panel.layout()
    assert isinstance(debug_layout, QVBoxLayout)
    assert debug_layout.count() == 3

    titles = []
    for index in range(debug_layout.count()):
        section = debug_layout.itemAt(index).widget()
        section_layout = section.layout()
        titles.append(section_layout.itemAt(0).widget().text())

    assert titles == ["Operations (log)", "Debug log", "Real-time data"]

    window.close()
    app.processEvents()


def test_metrics_log_does_not_repeat_invalid_nan_values(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)

    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow(debug=True)

    nan_value = float("nan")
    snapshot = MetricsSnapshot(
        temperatures=(nan_value, None, 35.6, 37.5),
        fan_speeds=(nan_value, None, None, None),
        voltage_v=0.1,
        current_ma=0.1,
    )

    window.on_metrics_received(snapshot)
    first_log = window.data_view.toPlainText()
    window.on_metrics_received(snapshot)
    second_log = window.data_view.toPlainText()

    assert first_log.count("Temp 1: NC") == 1
    assert first_log.count("Fan 1: NC") == 1
    assert second_log == first_log

    window.close()
    app.processEvents()


def test_logs_include_timestamp_prefix(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("pcwcc.core.load_or_create_host_key", lambda: object())
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)

    def _submit_noop(self, coro):
        coro.close()
        return None

    class _FakeDateTime:
        @staticmethod
        def now() -> datetime:
            return datetime(2026, 6, 1, 12, 34, 56)

    monkeypatch.setattr(gui_module.BleWorker, "submit", _submit_noop)
    monkeypatch.setattr(window_updates_module, "datetime", _FakeDateTime)

    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow(debug=True)

    window.on_log("hello")
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window.on_operation_status(
        gui_module.OperationStatus(op_type=1, state=1, error=""), window.model.state.connection_id
    )
    window.on_metrics_received(
        MetricsSnapshot(
            temperatures=(38.0, None, None, None),
            fan_speeds=(500.0, None, None, None),
            voltage_v=12.0,
            current_ma=25.0,
        )
    )

    prefix = "[2026-06-01 12:34:56]"
    assert window.debug_log_view.toPlainText().splitlines()[-1] == f"{prefix} hello"
    assert prefix in window.op_log_view.toPlainText()
    assert prefix in window.data_view.toPlainText()

    window.close()
    app.processEvents()


def _worker_with_ble_stub(monkeypatch):
    worker = gui_module.BleWorker()
    backend = SimpleNamespace(client=None, output_limit_notifications_active=False)
    callbacks = []

    async def connect_raw(device, *, connect_timeout, disconnected_callback):
        backend.client = SimpleNamespace(is_connected=True)
        callbacks.append((disconnected_callback, backend.client))

    async def disconnect():
        if backend.client is not None:
            backend.client.is_connected = False
            backend.client = None

    backend.connect_raw = connect_raw
    backend.disconnect = disconnect
    for method in (
        "read_params", "read_fan_status", "read_device_status",
        "read_operation_status", "read_metrics_snapshot", "start_metrics_notify",
        "start_params_notify", "start_fan_status_notify", "start_device_status_notify",
        "start_operation_status_notify", "stop_metrics_notify", "stop_params_notify",
        "stop_fan_status_notify", "stop_device_status_notify", "stop_operation_status_notify",
        "start_output_limit_notify", "read_output_limit", "stop_output_limit_notify",
        "start_fan_mode_status_notify", "read_fan_mode_status", "stop_fan_mode_status_notify",
    ):
        setattr(backend, method, AsyncMock(return_value=None))
    worker.core = backend
    monkeypatch.setattr(worker, "_do_auth", AsyncMock())
    return worker, callbacks


@pytest.mark.asyncio
async def test_worker_scopes_backend_disconnect_to_its_session(monkeypatch) -> None:
    worker, callbacks = _worker_with_ble_stub(monkeypatch)
    model = AppModel()
    device = DeviceInfo(name="Fan", address="AA:BB")
    model.set_selection(SelectionSource.PAIRED, device)
    states = []

    def apply_event(event):
        model.handle_connection_event(event)
        states.append((event.kind, model.state.conn, model.state.busy))

    worker.connection_event.connect(apply_event)
    first = model.dispatch(Action.CONNECT)
    assert first is not None
    assert await worker.connect_device(device, first.connection_id)
    assert states[0] == (ConnEventKind.OLD_SESSION_CLOSED, ConnState.CONNECTING, True)

    assert model.dispatch(Action.DISCONNECT) is not None
    await worker.disconnect_device(first.connection_id)
    second = model.dispatch(Action.CONNECT)
    assert second is not None
    try:
        assert await worker.connect_device(device, second.connection_id)
        old_callback, old_client = callbacks[0]
        old_callback(old_client)
        await asyncio.sleep(0)
        assert model.state.conn == ConnState.CONNECTED
        assert model.state.connection_id is second.connection_id

        callback, client = callbacks[1]
        client.is_connected = False
        callback(client)
        await asyncio.sleep(0)
        assert model.state.conn == ConnState.DISCONNECTED
    finally:
        await worker.disconnect_device()


@pytest.mark.asyncio
async def test_connect_reads_runtime_status_once_and_keeps_newer_notification(monkeypatch) -> None:
    worker, _ = _worker_with_ble_stub(monkeypatch)
    model = AppModel()
    device = DeviceInfo(name="Fan", address="AA:BB")
    model.set_selection(SelectionSource.PAIRED, device)
    command = model.dispatch(Action.CONNECT)
    assert command is not None
    worker.connection_event.connect(model.handle_connection_event)
    worker.output_limit_received.connect(model.handle_output_limit)
    worker.fan_mode_status_received.connect(model.handle_fan_mode_status)
    received = OutputLimitStatus(70, 60, OutputLimitState.LIMITED)

    async def read_limit(*, timeout):
        worker.core.start_output_limit_notify.await_args.args[0](received)
        return OutputLimitStatus(100, 100, OutputLimitState.NORMAL)

    worker.core.read_output_limit.side_effect = read_limit
    assert await worker.connect_device(device, command.connection_id)
    try:
        await asyncio.sleep(0)
        assert model.state.output_limit == received
        worker.core.read_output_limit.assert_awaited_once()
        worker.core.read_fan_mode_status.assert_awaited_once()
        worker.core.start_output_limit_notify.assert_awaited_once()
        worker.core.start_fan_mode_status_notify.assert_awaited_once()
    finally:
        await worker.disconnect_device()


@pytest.mark.asyncio
async def test_auto_stop_during_authorization_cleans_up_before_unlock(monkeypatch) -> None:
    worker, _ = _worker_with_ble_stub(monkeypatch)
    model = AppModel()
    device = DeviceInfo(name="Fan", address="AA:BB")
    monkeypatch.setattr(worker, "_load_saved_devices", lambda: [device])
    entered_auth = asyncio.Event()

    async def authorize():
        entered_auth.set()
        await asyncio.Event().wait()

    monkeypatch.setattr(worker, "_do_auth", authorize)
    worker.connection_event.connect(model.handle_connection_event)
    stop = asyncio.Event()
    assert model.start_auto(stop)
    run = asyncio.create_task(worker.auto_connect_saved(stop))
    try:
        await asyncio.wait_for(entered_auth.wait(), timeout=1)
        assert model.state.conn == ConnState.CONNECTING
        model.stop_auto()
        stop.set()
        assert model.state.conn == ConnState.DISCONNECTING
        assert model.state.busy
        await asyncio.wait_for(run, timeout=1)
        assert worker.core.client is None
        assert model.state.conn == ConnState.DISCONNECTED
        assert not model.state.busy
    finally:
        stop.set()
        if not run.done():
            await asyncio.wait_for(run, timeout=1)


@pytest.mark.asyncio
async def test_worker_reads_params_result_after_acknowledged_apply(monkeypatch) -> None:
    worker, _ = _worker_with_ble_stub(monkeypatch)
    order = []
    status = ParamsStatus(ok=True, status=0, field_id=None)
    worker.params_command_done.connect(lambda *result: order.append(result))

    async def write(params, *, timeout):
        order.append("write")

    async def apply(*, timeout):
        order.append("apply")

    async def read(*, timeout):
        order.append("read")
        return status

    worker.core.write_params = write
    worker.core.apply_params = apply
    worker.core.read_params_status = read
    command_id = object()
    await worker.apply_params(DEFAULT_PARAMS, command_id)
    assert order == ["write", "apply", "read", (command_id, status, "")]


@pytest.mark.asyncio
async def test_save_maximum_refreshes_both_values_from_controller(monkeypatch) -> None:
    worker, _ = _worker_with_ble_stub(monkeypatch)
    order = []
    worker.output_limit_received.connect(lambda status, _: order.append(status.configured))
    worker.params_received.connect(lambda params: order.append(params.fan_min_speed))
    worker.output_limit_command_done.connect(lambda _, error: order.append(error))

    async def save(maximum, *, timeout):
        order.append(f"save {maximum}")

    worker.core.set_output_max = save
    worker.core.read_output_limit = AsyncMock(return_value=OutputLimitStatus(70, 70, OutputLimitState.NORMAL))
    worker.core.read_params = AsyncMock(return_value=replace(DEFAULT_PARAMS, fan_min_speed=70))

    await worker.change_output_limit(object(), 70)

    assert order == ["save 70", 70, 70, ""]


@pytest.mark.asyncio
async def test_fast_disconnect_cancels_operation_before_closing_link(monkeypatch) -> None:
    worker, _ = _worker_with_ble_stub(monkeypatch)
    device = DeviceInfo(name="Fan", address="AA:BB")
    assert await worker.connect_device(device, object())
    order = []
    disconnect = worker.core.disconnect

    async def cancel(*, timeout):
        order.append("cancel")
        assert worker.core.client is not None

    async def close():
        order.append("disconnect")
        await disconnect()

    worker.core.cancel_operation = cancel
    worker.core.disconnect = close
    await worker.disconnect_device(fast=True)
    assert order == ["cancel", "disconnect"]
    assert worker.core.client is None


@pytest.mark.asyncio
async def test_output_limit_ble_read_and_save(monkeypatch) -> None:
    core = BleAppCore()
    read = AsyncMock(return_value=bytes((1, 60, 40, 5)))
    write = AsyncMock()
    monkeypatch.setattr(core, "_read_char", read)
    monkeypatch.setattr(core, "_write_char", write)
    from pcwcc.core_logic.protocol import UUID_CONFIG_OUTPUT_LIMIT, UUID_CONFIG_OUTPUT_RANGE

    status = await core.read_output_limit(timeout=1.0)
    assert (status.configured, status.effective, status.state) == (60, 40, 5)
    read.assert_awaited_once_with(UUID_CONFIG_OUTPUT_RANGE, timeout=1.0)
    await core.set_output_max(60, timeout=1.0)
    write.assert_awaited_with(UUID_CONFIG_OUTPUT_LIMIT, bytes((1, 1, 60)), response=True, timeout=1.0)
    with pytest.raises(ValueError):
        await core.set_output_max(0, timeout=1.0)


@pytest.mark.asyncio
@pytest.mark.parametrize("result", [2, 3])
async def test_output_limit_reset_uses_matching_terminal_notification(monkeypatch, result) -> None:
    from pcwcc.core_logic.protocol import UUID_CONFIG_OUTPUT_LIMIT, UUID_CONFIG_OUTPUT_RANGE

    core = BleAppCore()
    services = {UUID_CONFIG_OUTPUT_RANGE: SimpleNamespace(properties=["read", "notify"])}
    core.client = SimpleNamespace(services=SimpleNamespace(get_characteristic=services.get))
    read = AsyncMock(return_value=bytes((3, 60, 40, 5, 255, 0, 255, 0)))
    start = AsyncMock()
    monkeypatch.setattr(core, "_start_notify", start)
    monkeypatch.setattr(core, "_read_char", read)
    await core.start_output_limit_notify(lambda status: None)
    notify = start.await_args.args[1]
    await core.read_output_limit(timeout=1.0)

    async def send(*args, **kwargs):
        notify(None, bytearray((3, 60, 60, 0, 255, 2, 255, 0)))
        assert not core._output_reset_waiter[1].done()
        notify(None, bytearray((3, 60, 40, 5, 0, 1, 255, 0)))
        assert not core._output_reset_waiter[1].done()
        notify(None, bytearray((3, 60, 60, 0, 0, result, 255, 0)))

    write = AsyncMock(side_effect=send)
    monkeypatch.setattr(core, "_write_char", write)
    if result == 2:
        reset = await core.reset_output_limit(timeout=1.0)
        assert (reset.effective, reset.reset_sequence) == (60, 0)
    else:
        with pytest.raises(RuntimeError, match="Temporary limit was not reset"):
            await core.reset_output_limit(timeout=1.0)
    assert core._output_reset_waiter is None
    write.assert_awaited_once_with(UUID_CONFIG_OUTPUT_LIMIT, bytes((1, 2, 0)), response=True, timeout=1.0)
    read.assert_awaited_once()


@pytest.mark.asyncio
async def test_output_range_reads_confirmed_minimum_and_preserves_legacy_status(monkeypatch) -> None:
    from bleak.exc import BleakCharacteristicNotFoundError
    from pcwcc.core_logic.protocol import UUID_CONFIG_OUTPUT_LIMIT, UUID_CONFIG_OUTPUT_RANGE

    core = BleAppCore()
    read = AsyncMock(side_effect=[
        bytes((3, 100, 58, 5, 0, 0, 58, 0)),
        bytes((3, 100, 0, 8, 0, 0, 255, 1)),
        BleakCharacteristicNotFoundError(UUID_CONFIG_OUTPUT_RANGE),
        bytes((2, 100, 58, 5, 0, 0)),
    ])
    monkeypatch.setattr(core, "_read_char", read)

    limited = await core.read_output_limit(timeout=1.0)
    assert (limited.temporary_minimum, limited.effective) == (58, 58)
    fault = await core.read_output_limit(timeout=1.0)
    assert fault.alert_without_load and fault.temporary_minimum is None
    legacy = await core.read_output_limit(timeout=1.0)
    assert legacy.effective == 58 and legacy.temporary_minimum is None
    assert read.await_args.args == (UUID_CONFIG_OUTPUT_LIMIT,)


@pytest.mark.asyncio
async def test_missing_saved_output_limit_can_be_repaired_from_window(monkeypatch) -> None:
    core = BleAppCore()
    monkeypatch.setattr(core, "_read_char", AsyncMock(return_value=bytes((2, 0, 0, 0, 0, 0))))
    status = await core.read_output_limit(timeout=1.0)

    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window.on_output_limit(status, window.model.state.connection_id)

    assert "saved limit unavailable" in window.output_limit_label.text()
    assert window.output_max_input.text() == "Unavailable"
    assert not window.output_max_save.isEnabled()
    window.output_max_input.setValue(60)
    assert window.output_max_save.isEnabled()
    window.close()
    app.processEvents()


def test_conflicting_limits_are_sent_to_controller_and_rejection_is_displayed(monkeypatch) -> None:
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window._set_params_fields(DEFAULT_PARAMS, save=False)
    minimum_field = next(item["widget"] for item in window.param_fields if item["spec"]["key"] == "fan_min_speed")
    minimum_field.setValue(65)
    sent = []
    monkeypatch.setattr(window.worker, "apply_params", lambda params, _: sent.append(params.fan_min_speed))
    monkeypatch.setattr(window.worker, "change_output_limit", lambda _, maximum: sent.append(maximum))
    monkeypatch.setattr(window, "_start_action", lambda *_: None)
    connection = window.model.state.connection_id

    window.on_output_limit(OutputLimitStatus(60, 60, OutputLimitState.NORMAL), connection)
    window.on_apply()

    assert sent == [65]
    assert window.apply_status_label.isHidden()
    window.on_params_command_done(
        window.model.state.command_id,
        ParamsStatus(ok=False, status=PARAM_STATUS_INVALID, field_id=PARAM_FIELD_FAN_MIN_SPEED),
        "",
    )
    assert minimum_field.value() == 65
    assert "controller caps the applied value at the saved maximum" in window.apply_status_label.text()
    assert window.apply_status_label.wordWrap()
    assert not window.apply_status_label.isHidden()

    window._set_params_fields(replace(DEFAULT_PARAMS, fan_min_speed=65), save=False)
    window.on_change_output_limit(60)
    assert sent == [65, 60]
    assert window.model.state.active_action == Action.SET_OUTPUT_MAX
    window.on_output_limit_command_done(window.model.state.command_id, "Maximum output was rejected.")
    assert window.apply_status_label.text() == "Maximum output was rejected."
    assert window.model.state.active_action is None
    window.close()
    app.processEvents()


def test_capped_profile_field_is_reported_as_applied(monkeypatch) -> None:
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    window._set_params_fields(DEFAULT_PARAMS, save=False)
    window.on_output_limit(OutputLimitStatus(70, 70, OutputLimitState.NORMAL), window.model.state.connection_id)
    window.output_max_input.setValue(80)
    assert "Saved maximum: 70%" in window.output_limit_label.text()

    monkeypatch.setattr(window.worker, "submit", lambda coro: coro.close())
    command = window.model.dispatch(Action.APPLY_PROFILE, slot=1)
    assert command is not None
    window.on_params_command_done(
        window.model.state.command_id,
        ProfileApplyResult(1, 0, 1 << PARAM_FIELD_FAN_MIN_SPEED, (ProfileSkipReason.OUTPUT_MAX,)),
        "",
    )
    assert "Profile applied (" in window.apply_status_label.text()
    assert "reduced to the saved maximum output" in window.apply_status_label.text()
    window.close()
    app.processEvents()


def test_maximum_save_refreshes_working_minimum_and_preserves_other_draft_fields(monkeypatch) -> None:
    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    monkeypatch.setattr(gui_module, "save_device_params", lambda *_: None)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    previous = replace(DEFAULT_PARAMS, fan_min_speed=80)
    window._set_params_fields(previous, save=False)
    window.on_output_limit(OutputLimitStatus(100, 100, OutputLimitState.NORMAL), window.model.state.connection_id)
    temp_field = next(item["widget"] for item in window.param_fields if item["spec"]["key"] == "fan_max_temp")
    temp_field.setValue(previous.fan_max_temp + 1)
    assert window.model.dispatch(Action.SET_OUTPUT_MAX) is not None

    window.on_params_received(replace(previous, fan_min_speed=70))

    assert window._device_params_snapshot.fan_min_speed == 70
    assert window._current_params().fan_min_speed == 70
    assert window._current_params().fan_max_temp == previous.fan_max_temp + 1
    assert "Base fan speed reduced to 70%" in window.apply_status_label.text()
    window.close()
    app.processEvents()


@pytest.mark.asyncio
async def test_set_output_max_reports_controller_validation_failure(monkeypatch) -> None:
    core = BleAppCore()
    write = AsyncMock(side_effect=RuntimeError("BLE write rejected"))
    read_status = AsyncMock(return_value=ParamsStatus(ok=False, status=PARAM_STATUS_INVALID, field_id=None))
    monkeypatch.setattr(core, "_write_char", write)
    monkeypatch.setattr(core, "read_params_status", read_status)

    with pytest.raises(RuntimeError, match="Maximum output was rejected"):
        await core.set_output_max(60, timeout=1.0)
    write.assert_awaited_once()
    read_status.assert_awaited_once_with(timeout=1.0)


@pytest.mark.asyncio
async def test_fan_mode_status_and_partial_profile_result(monkeypatch) -> None:
    from pcwcc.core_logic.models import ProfileSkipReason, ThermalInputState
    from pcwcc.core_logic.protocol import UUID_CONFIG_FAN_MODE_STATUS, UUID_CONFIG_PROFILE_RESULT

    core = BleAppCore()
    read = AsyncMock(side_effect=[
        bytes((1, 1, 0, 2)),
        bytes((1, 2, 0, 2, 0, 2, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0)),
    ])
    monkeypatch.setattr(core, "_read_char", read)
    mode = await core.read_fan_mode_status(timeout=1.0)
    assert (mode.selected, mode.effective, mode.thermal_input) == (1, 0, ThermalInputState.ADC_OFFLINE)
    result = await core.read_profile_apply_result(2, timeout=1.0)
    assert result.ok and result.applied_mask == 2
    assert result.skipped[0] == ProfileSkipReason.OUTPUT_MAX
    assert result.skipped[5] == ProfileSkipReason.NTC_UNAVAILABLE
    assert [call.args[0] for call in read.await_args_list] == [
        UUID_CONFIG_FAN_MODE_STATUS, UUID_CONFIG_PROFILE_RESULT,
    ]


def test_fan_mode_display_is_scoped_to_current_connection(monkeypatch) -> None:
    from pcwcc.core_logic.models import FanModeStatus, ThermalInputState

    monkeypatch.setattr(gui_module.BleWorker, "start", lambda self: None)
    monkeypatch.setattr(gui_module.BleWorker, "stop", lambda self: None)
    app = QApplication.instance() or QApplication([])
    window = gui_module.MainWindow()
    window.model.handle_connection_event(
        _connected_event(window.model, DeviceInfo(name="Test", address="AA:BB"))
    )
    connection_id = window.model.state.connection_id
    window.on_fan_mode_status(FanModeStatus(1, 0, ThermalInputState.NTC_UNAVAILABLE), connection_id)
    assert "Always on" in window.effective_fan_mode_label.text()
    assert "control thermistor unavailable" in window.effective_fan_mode_label.text()
    window.on_fan_mode_status(FanModeStatus(1, 0, ThermalInputState.ADC_OFFLINE), connection_id)
    assert "ADS1115 unavailable" in window.effective_fan_mode_label.text()
    window.on_fan_mode_status(FanModeStatus(1, 1, ThermalInputState.READY), object())
    assert "ADS1115 unavailable" in window.effective_fan_mode_label.text()
    window.on_fan_mode_status(FanModeStatus(1, 1, ThermalInputState.READY), connection_id)
    assert "Current fan mode: By temperature sensor" == window.effective_fan_mode_label.text()
    window.close()
    app.processEvents()


@pytest.mark.asyncio
async def test_write_params_sends_payload_only_to_config_params() -> None:
    from pcwcc.core_logic.protocol import UUID_CONFIG_PARAMS

    core = BleAppCore()
    client = AsyncMock()
    core.client = client
    await core.write_params(DEFAULT_PARAMS, timeout=1.0, mask=1)
    client.write_gatt_char.assert_awaited_once()
    args, kwargs = client.write_gatt_char.await_args
    assert args[0] == UUID_CONFIG_PARAMS and len(args) == 2
    assert args[1][1:3] == bytes((1, 0))
    assert kwargs == {"response": True}
