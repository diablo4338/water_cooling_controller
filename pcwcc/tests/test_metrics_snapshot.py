import struct
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
from pcwcc.core_logic.models import DEFAULT_PARAMS
from pcwcc.gui_logic import window_updates as window_updates_module
from pcwcc.presentation import Action


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
    window.on_connection_state(True, device)
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
    window.on_connection_state(True, device)
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

    window.on_connection_state(True, device)

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
    monkeypatch.setattr(window.worker, "apply_profile", lambda index: applied_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    monkeypatch.setattr(
        window, "_dispatch_action", lambda action: dispatched_actions.append(action)
    )
    window.model.set_connected(True, DeviceInfo(name="Test", address="AA:BB"))
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
    monkeypatch.setattr(
        window, "_dispatch_action", lambda action: dispatched_actions.append(action)
    )
    window.model.set_connected(True, DeviceInfo(name="Test", address="AA:BB"))
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

    assert window._profile_save_pending == 2
    monkeypatch.setattr(window.worker, "save_profile", lambda index: saved_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    window.on_params_status(ParamsStatus(ok=True, status=0, field_id=None))
    assert saved_profiles == [2]
    assert dispatched_actions == [Action.APPLY]

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
    monkeypatch.setattr(window.model, "dispatch", lambda action: None)

    window.on_apply()

    assert window._profile_save_pending is None

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
    monkeypatch.setattr(window.worker, "apply_profile", lambda index: applied_profiles.append(index))
    monkeypatch.setattr(window, "_submit_profile_action", lambda action: action())
    monkeypatch.setattr(
        window, "_dispatch_action", lambda action: dispatched_actions.append(action)
    )
    window.model.set_connected(True, DeviceInfo(name="Test", address="AA:BB"))
    window._device_profile_slots = {1}
    window._set_params_fields(DEFAULT_PARAMS, save=False)
    window._refresh_profile_buttons()
    window._apply_ui()

    window.profile_buttons[0].click()
    assert applied_profiles == [1]
    window.on_profile_action_done(PROFILE_ACTION_APPLY, 1, True, "")
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
    window.model.set_connected(True, DeviceInfo(name="Test", address="AA:BB"))
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
    window.on_operation_status(gui_module.OperationStatus(op_type=1, state=1, error=""))
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
