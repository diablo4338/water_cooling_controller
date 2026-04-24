from __future__ import annotations

import math

from ..core import (
    DeviceParams,
    FAN_CONTROL_DC,
    FAN_MODE_CONTINUOUS,
    load_device_params,
    save_device_params,
)
from .constants import FAN_MONITORING_KEYS, PARAM_FIELDS


class MainWindowParamsMixin:
    def _set_fan_status_indicator(self, channel: int, color: str) -> None:
        if channel < len(self.fan_status_indicators):
            self.fan_status_indicators[channel].setStyleSheet(
                f"background:{color}; border:1px solid #111827;"
            )

    def _set_fan_status_tooltip(self, channel: int, text: str) -> None:
        if channel < len(self.fan_status_indicators):
            self.fan_status_indicators[channel].setToolTip(text)

    def _set_device_status_indicator(self, color: str) -> None:
        if self.device_status_indicator is not None:
            self.device_status_indicator.setStyleSheet(
                f"background:{color}; border:1px solid #111827;"
            )

    def _set_temp_indicator(self, channel: int, color: str, text: str) -> None:
        if channel < len(self.temp_indicators):
            self.temp_indicators[channel].setStyleSheet(
                f"background:{color}; border:1px solid #111827;"
            )
            self.temp_indicators[channel].setToolTip(text)

    def _device_params_for_metrics(self) -> DeviceParams:
        if self._device_params_snapshot is not None:
            return self._device_params_snapshot
        device = self.model.state.connected_device
        if device:
            return load_device_params(device.address)
        return DeviceParams(
            fan_min_speed=10,
            fan_control_type=FAN_CONTROL_DC,
            fan_max_temp=45,
            fan_off_delta=2,
            fan_start_temp=35,
            fan_mode=FAN_MODE_CONTINUOUS,
            fan_monitoring_enabled=True,
            fan2_monitoring_enabled=True,
            fan3_monitoring_enabled=True,
            fan4_monitoring_enabled=True,
        )

    def _refresh_temp_indicators(self) -> None:
        max_temp = float(self._device_params_for_metrics().fan_max_temp)
        for channel, value in enumerate(self.temp_values):
            if value is None or not math.isfinite(value):
                self._set_temp_indicator(channel, "#6b7280", "NC")
                continue
            delta = max_temp - value
            if delta > 5.0:
                self._set_temp_indicator(channel, "#22c55e", f"{delta:.2f}C below max")
            elif value < max_temp:
                self._set_temp_indicator(channel, "#eab308", f"{delta:.2f}C below max")
            else:
                self._set_temp_indicator(channel, "#ef4444", "At or above max")

    def _reset_params_fields(self) -> None:
        self._params_update_lock = True
        try:
            for item in self.param_fields:
                spec = item["spec"]
                widget = item["widget"]
                if spec["kind"] == "enum":
                    widget.setCurrentIndex(0)
                else:
                    widget.setValue(widget.minimum())
                widget.setEnabled(False)
            for checkbox in self.fan_monitor_checkboxes:
                checkbox.setChecked(False)
                checkbox.setEnabled(False)
        finally:
            self._params_update_lock = False

    def _set_params_fields(self, params: DeviceParams, save: bool = True) -> None:
        if len(self.param_fields) != len(PARAM_FIELDS) or len(self.fan_monitor_checkboxes) != 4:
            return
        self._device_params_snapshot = params
        self._params_update_lock = True
        try:
            for item in self.param_fields:
                spec = item["spec"]
                widget = item["widget"]
                value = getattr(params, spec["key"])
                if spec["kind"] == "enum":
                    idx = widget.findData(int(value))
                    widget.setCurrentIndex(idx if idx >= 0 else 0)
                else:
                    widget.setValue(value)
                widget.setEnabled(True)
            for idx, key in enumerate(FAN_MONITORING_KEYS):
                self.fan_monitor_checkboxes[idx].setChecked(bool(getattr(params, key)))
                self.fan_monitor_checkboxes[idx].setEnabled(idx > 0)
        finally:
            self._params_update_lock = False
        if save and self.model.state.connected_device:
            save_device_params(self.model.state.connected_device.address, params)
        self._refresh_temp_indicators()

    def _current_params(self) -> DeviceParams:
        if len(self.param_fields) != len(PARAM_FIELDS) or len(self.fan_monitor_checkboxes) != 4:
            device = self.model.state.connected_device
            return load_device_params(device.address) if device else self._device_params_for_metrics()
        values: dict[str, object] = {}
        for item in self.param_fields:
            spec = item["spec"]
            widget = item["widget"]
            value = int(widget.currentData()) if spec["kind"] == "enum" else int(widget.value())
            values[spec["key"]] = value
        for idx, key in enumerate(FAN_MONITORING_KEYS):
            values[key] = self.fan_monitor_checkboxes[idx].isChecked()
        return DeviceParams(
            fan_min_speed=int(values["fan_min_speed"]),
            fan_control_type=int(values["fan_control_type"]),
            fan_max_temp=int(values["fan_max_temp"]),
            fan_off_delta=int(values["fan_off_delta"]),
            fan_start_temp=int(values["fan_start_temp"]),
            fan_mode=int(values["fan_mode"]),
            fan_monitoring_enabled=bool(values["fan_monitoring_enabled"]),
            fan2_monitoring_enabled=bool(values["fan2_monitoring_enabled"]),
            fan3_monitoring_enabled=bool(values["fan3_monitoring_enabled"]),
            fan4_monitoring_enabled=bool(values["fan4_monitoring_enabled"]),
        )

    def _on_params_changed(self, _) -> None:
        if self._params_update_lock:
            return
        params = self._current_params()
        device = self.model.state.connected_device
        if device is None or self.model.state.conn != self.ConnState.CONNECTED:
            return
        save_device_params(device.address, params)
        fut = self.worker.submit(self.worker.write_params(params))
        if fut is None:
            self.on_log("Failed to send parameters (worker not ready).")
        self._refresh_temp_indicators()
