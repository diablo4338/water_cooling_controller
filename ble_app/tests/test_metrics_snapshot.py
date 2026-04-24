import os
import struct

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("XDG_CONFIG_HOME", "/tmp")

import pytest
from PySide6.QtWidgets import QApplication

from ble_app.core import (
    BleAppCore,
    DeviceInfo,
    DeviceParams,
    MetricsSnapshot,
    UUID_FAN2_SPEED_VALUE,
    UUID_FAN3_SPEED_VALUE,
    UUID_FAN4_SPEED_VALUE,
    UUID_FAN_SPEED_VALUE,
    UUID_TEMP0_VALUE,
    UUID_TEMP1_VALUE,
    UUID_TEMP2_VALUE,
    UUID_TEMP3_VALUE,
)
from ble_app import gui as gui_module


class _FakeClient:
    def __init__(self, payloads: dict[str, bytes]) -> None:
        self._payloads = payloads

    async def read_gatt_char(self, uuid: str) -> bytes:
        return self._payloads[uuid]


@pytest.mark.asyncio
async def test_read_metrics_snapshot_reads_temperatures_and_fans(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("ble_app.core.load_or_create_host_key", lambda: object())
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
        }
    )

    snapshot = await core.read_metrics_snapshot(timeout=1.0, retries=0)

    assert snapshot == MetricsSnapshot(
        temperatures=(11.5, 22.5, 33.5, 44.5),
        fan_speeds=(1000.0, 1100.0, 1200.0, 1300.0),
    )


def test_temp_indicator_uses_device_params_snapshot_while_form_is_dirty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("ble_app.core.load_or_create_host_key", lambda: object())
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
    )
    window.on_connection_state(True, device)
    window.on_params_received(params)
    window.on_metrics_received(
        MetricsSnapshot(
            temperatures=(44.0, None, None, None),
            fan_speeds=(None, None, None, None),
        )
    )

    assert "#eab308" in window.temp_indicators[0].styleSheet()

    fan_max_temp = next(
        item["widget"] for item in window.param_fields if item["spec"]["key"] == "fan_max_temp"
    )
    fan_max_temp.setValue(30)

    assert "#eab308" in window.temp_indicators[0].styleSheet()

    window.close()
    app.processEvents()
