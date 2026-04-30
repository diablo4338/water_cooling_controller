from __future__ import annotations

import math
from typing import Optional

from PySide6.QtCore import QTimer, Slot
from PySide6.QtWidgets import QListWidgetItem

from ..core import (
    DEVICE_ERROR_NONE,
    DEVICE_STATE_ERROR,
    DEVICE_STATE_OK,
    FAN_STATE_IDLE,
    FAN_STATE_IN_SERVICE,
    FAN_STATE_RUNNING,
    FAN_STATE_STALL,
    FAN_STATE_STARTING,
    OP_STATE_DONE,
    OP_STATE_ERROR,
    OP_STATE_IDLE,
    OP_STATE_IN_SERVICE,
    OP_STATE_NAMES,
    OP_TYPE_FAN_CALIBRATION,
    OP_TYPE_NAMES,
    OP_TYPE_NONE,
    OP_TYPE_SETUP_FANS,
    DeviceInfo,
    DeviceParams,
    DeviceStatus,
    FanStatus,
    MetricsSnapshot,
    OperationStatus,
    ParamsStatus,
    update_paired_last_connected,
)
from .constants import PARAM_ERROR_MESSAGES, PARAM_LABELS_BY_ID, USER_ROLE


class MainWindowUpdateMixin:
    def _clear_op_log(self) -> None:
        if self.op_log_view is not None:
            self.op_log_view.clear()

    @Slot(list)
    def on_scan_results(self, devices: list) -> None:
        if self.model.state.active_action != self.Action.SCAN:
            return
        self.found_list.clear()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(USER_ROLE, dev)
            self.found_list.addItem(item)
        self.model.set_found_devices(devices)
        if self.model.state.selected_source == self.SelectionSource.FOUND:
            self.model.set_selection(None, None)
        self._finish_action(self.Action.SCAN)

    @Slot(str)
    def on_log(self, message: str) -> None:
        self.model.set_status(message)
        if self.debug_enabled and self.debug_log_view is not None:
            self.debug_log_view.append(message)
        print(message, flush=True)
        self._apply_ui()

    def _apply_temp_value(self, channel: int, value: Optional[float], append_data: bool) -> None:
        if not (0 <= channel < len(self.temp_fields)):
            return
        is_nc = value is None or not math.isfinite(value)
        self.temp_values[channel] = None if is_nc else value
        prev_nc = self._temp_is_nc[channel]
        if prev_nc is not None and prev_nc != is_nc:
            self.on_log(f"Channel {channel + 1}: {'NC (no sensor)' if is_nc else 'online'}")
        self._temp_is_nc[channel] = is_nc
        text = f"{value:.2f}" if not is_nc and value is not None else "NC"
        self.temp_fields[channel].setText(text)
        if append_data:
            self.data_view.append(f"Temp {channel + 1}: {text}")

    def _apply_fan_value(self, channel: int, value: Optional[float], append_data: bool) -> None:
        if channel >= len(self.fan_fields):
            return
        is_nc = value is None or (not math.isfinite(value)) or value <= 0.0
        if self._fan_is_nc[channel] is not None and self._fan_is_nc[channel] != is_nc:
            self.on_log(f"Fan {channel + 1}: {'NC (stopped)' if is_nc else 'online'}")
        self._fan_is_nc[channel] = is_nc
        text = "NC" if is_nc or value is None else f"{value:.0f}"
        self.fan_fields[channel].setText(text)
        if append_data:
            self.data_view.append(f"Fan {channel + 1}: {text}")

    def _apply_power_value(self, field, label: str, value: Optional[float], unit: str, decimals: int, append_data: bool) -> None:
        if field is None:
            return
        is_nc = value is None or not math.isfinite(value)
        text = "NC" if is_nc else f"{value:.{decimals}f}"
        field.setText(text)
        if append_data:
            self.data_view.append(f"{label}: {text} {unit}")

    @Slot(object)
    def on_metrics_received(self, snapshot: MetricsSnapshot) -> None:
        prev_snapshot = self.metrics_snapshot
        self.metrics_snapshot = snapshot
        for channel, value in enumerate(snapshot.temperatures):
            self._apply_temp_value(channel, value, append_data=prev_snapshot.temperatures[channel] != value)
        for channel, value in enumerate(snapshot.fan_speeds):
            self._apply_fan_value(channel, value, append_data=prev_snapshot.fan_speeds[channel] != value)
        self.metrics_chart.add_sample(snapshot.fan_speeds[0], snapshot.temperatures[3])
        self._apply_power_value(
            self.voltage_field,
            "Voltage",
            snapshot.voltage_v,
            "V",
            2,
            append_data=prev_snapshot.voltage_v != snapshot.voltage_v,
        )
        self._apply_power_value(
            self.current_field,
            "Current",
            snapshot.current_ma,
            "mA",
            1,
            append_data=prev_snapshot.current_ma != snapshot.current_ma,
        )
        self._refresh_temp_indicators()

    @Slot(object)
    def on_params_received(self, params: DeviceParams) -> None:
        self._set_params_fields(params, save=True)
        if self.model.state.active_action == self.Action.DISCARD:
            self._finish_action(self.Action.DISCARD)

    @Slot(object)
    def on_params_status(self, status: ParamsStatus) -> None:
        if self.model.state.active_action == self.Action.APPLY:
            QTimer.singleShot(1000, lambda: self._finish_action(self.Action.APPLY))
        if status.ok:
            self.on_log("Parameters applied")
            self.worker.submit(self.worker.read_params_snapshot())
            return
        if status.status == self.PARAM_STATUS_BUSY:
            self.on_log("Parameters not applied: device busy")
            return
        field_label = (
            PARAM_LABELS_BY_ID.get(status.field_id, "unknown field")
            if status.field_id is not None
            else "unknown field"
        )
        self.on_log(f"Parameter error: {PARAM_ERROR_MESSAGES.get(status.field_id, field_label)}")

    @Slot(object)
    def on_fan_status(self, status: FanStatus) -> None:
        for channel, state in enumerate(status.states):
            if channel >= len(self.fan_status_indicators):
                continue
            if state == FAN_STATE_IDLE:
                self._set_fan_status_indicator(channel, "#22c55e")
                self._set_fan_status_tooltip(channel, "IDLE")
            elif state == FAN_STATE_STARTING:
                self._set_fan_status_indicator(channel, "#eab308")
                self._set_fan_status_tooltip(channel, "STARTING")
            elif state == FAN_STATE_RUNNING:
                self._set_fan_status_indicator(channel, "#eab308")
                self._set_fan_status_tooltip(channel, "RUNNING")
            elif state == FAN_STATE_STALL:
                self._set_fan_status_indicator(channel, "#ef4444")
                self._set_fan_status_tooltip(channel, "STALL")
            elif state == FAN_STATE_IN_SERVICE:
                tooltip = f"IN_SERVICE ({status.op_label})" if status.op_type != OP_TYPE_NONE else "IN_SERVICE"
                self._set_fan_status_tooltip(channel, tooltip)
                self._set_fan_status_indicator(channel, "#3b82f6")
        self._apply_ui()

    @Slot(object)
    def on_device_status(self, status: DeviceStatus) -> None:
        if self.device_status_field is None:
            return
        if status.state == DEVICE_STATE_OK:
            self.device_status_field.setText("OK")
            self._set_device_status_indicator("#22c55e")
        elif status.state == DEVICE_STATE_ERROR:
            if status.error_mask == DEVICE_ERROR_NONE:
                self.device_status_field.setText("ERROR")
            else:
                self.device_status_field.setText(f"ERROR ({', '.join(status.error_labels)})")
            self._set_device_status_indicator("#ef4444")
        else:
            self.device_status_field.setText(status.label)
            self._set_device_status_indicator("#6b7280")
        self._apply_ui()

    @Slot(object)
    def on_operation_status(self, status: OperationStatus) -> None:
        op_label = OP_TYPE_NAMES.get(status.op_type, f"OP{status.op_type}")
        state_label = OP_STATE_NAMES.get(status.state, "UNKNOWN")
        is_noop_idle = status.op_type == OP_TYPE_NONE and status.state == OP_STATE_IDLE
        if status.state == OP_STATE_IN_SERVICE and not self._operation_active:
            self._clear_op_log()
        if self.op_log_view is not None and not is_noop_idle:
            self.op_log_view.append(f"{op_label}: {status.error or state_label}")
        op_action = None
        if status.op_type == OP_TYPE_SETUP_FANS:
            op_action = self.Action.SETUP_FANS
        elif status.op_type == OP_TYPE_FAN_CALIBRATION:
            op_action = self.Action.SETUP_FANS if self.model.state.active_action == self.Action.SETUP_FANS else self.Action.CALIBRATE
        if status.state == OP_STATE_IN_SERVICE:
            if not self._operation_active:
                self.on_log(f"Operation started: {op_label}")
            self._operation_active = True
        elif status.state == OP_STATE_DONE:
            self.on_log(f"Operation completed: {op_label}")
            self._operation_active = False
            if op_action:
                self._finish_action(op_action)
                self.worker.submit(self.worker.read_params_snapshot())
        elif status.state == OP_STATE_ERROR:
            err_text = status.error or "unknown error"
            if err_text.strip().lower() == "busy":
                self.on_log(f"Operation busy: {op_label}")
                self._operation_active = True
            else:
                self.on_log(f"Operation error {op_label}: {err_text}")
                self._operation_active = False
                if op_action:
                    self._finish_action(op_action)
        elif status.state == OP_STATE_IDLE:
            self._operation_active = False
            if op_action:
                self._finish_action(op_action)
        else:
            self.on_log(f"Operation {op_label}: {state_label}")
        self._apply_ui()

    @Slot()
    def on_apply_done(self) -> None:
        pass

    @Slot(bool, str, object, object)
    def on_pairing_result(self, ok: bool, message: str, device: DeviceInfo, k_hex: Optional[str]) -> None:
        if ok:
            if k_hex:
                self._add_paired(device, k_hex)
            self._refresh_paired_list()
            self.found_list.clear()
            self.model.clear_found_devices()
        self.on_log(message)
        if self.model.state.active_action == self.Action.PAIR:
            self._finish_action(self.Action.PAIR)

    @Slot(bool, object)
    def on_connection_state(self, connected: bool, device: Optional[DeviceInfo]) -> None:
        if connected and device:
            self.model.set_connected(True, device)
            update_paired_last_connected(device.address)
            self._lock_selection_to_connected()
            if self.model.state.active_action == self.Action.CONNECT:
                self._finish_action(self.Action.CONNECT)
            self.on_log(f"Connected to {device.name}")
            return
        already_disconnected = self.model.state.conn == self.ConnState.DISCONNECTED
        self.model.set_connected(False, None)
        self._clear_paired_selection()
        self._device_params_snapshot = None
        self._reset_temp_fields()
        self._reset_params_fields()
        if self.model.state.active_action == self.Action.DISCONNECT:
            self._finish_action(self.Action.DISCONNECT)
        elif self.model.state.active_action == self.Action.CONNECT:
            self._finish_action(self.Action.CONNECT)
        if not already_disconnected:
            self.on_log("Disconnected")

    def _reset_temp_fields(self) -> None:
        self.metrics_snapshot = self.MetricsSnapshot.empty()
        for field in self.temp_fields:
            field.setText("—")
        self.temp_values = [None] * len(self.temp_fields)
        for channel in range(len(self.temp_indicators)):
            self._set_temp_indicator(channel, "#6b7280", "—")
        self._temp_is_nc = [None] * len(self.temp_fields)
        for field in self.fan_fields:
            field.setText("—")
        if self.voltage_field is not None:
            self.voltage_field.setText("—")
        if self.current_field is not None:
            self.current_field.setText("—")
        self._fan_is_nc = [None] * len(self.fan_fields)
        for channel in range(len(self.fan_status_indicators)):
            self._set_fan_status_indicator(channel, "#6b7280")
            self._set_fan_status_tooltip(channel, "—")
        if self.device_status_field is not None:
            self.device_status_field.setText("—")
        self._set_device_status_indicator("#6b7280")
        self._operation_active = False
        self.metrics_chart.clear()
