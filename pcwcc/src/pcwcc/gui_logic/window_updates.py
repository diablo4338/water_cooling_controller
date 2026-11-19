from __future__ import annotations

from datetime import datetime
import math
from typing import Optional

from PySide6.QtCore import Slot
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
    OP_TYPE_NAMES,
    OP_TYPE_NONE,
    PROFILE_ACTION_CLEAR,
    PROFILE_ACTION_SAVE_CURRENT,
    PARAM_STATUS_STORAGE_ERROR,
    DeviceInfo,
    DeviceParams,
    DeviceStatus,
    FanStatus,
    MetricsSnapshot,
    OperationStatus,
    ParamsStatus,
    load_device_params,
    load_metrics_history,
    load_metrics_history_all,
    save_metrics_history,
    update_paired_last_connected,
)
from .constants import PARAM_ERROR_MESSAGES, PARAM_LABELS_BY_ID, USER_ROLE

from ..core_logic.models import FanModeStatus, OutputLimitState, ProfileApplyResult, ProfileSkipReason
from ..core_logic.protocol import (
    DEVICE_ERROR_INA_OFFLINE,
    PARAM_FIELD_FAN_MIN_SPEED,
    PARAM_STATUS_ADC_UNAVAILABLE,
    PARAM_STATUS_NTC_UNAVAILABLE,
    PARAM_STATUS_OUTPUT_ADJUSTING,
)
from ..presentation import ConnEvent, ConnEventKind, PROFILE_COMMANDS, operation_running


class MainWindowUpdateMixin:
    def _set_status_message(self, message: str) -> None:
        self.model.set_status(message)
        self._apply_ui()

    @staticmethod
    def _timestamp() -> str:
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    def _format_log_entry(self, message: str) -> str:
        return f"[{self._timestamp()}] {message}"

    @staticmethod
    def _metric_value_changed(previous: Optional[float], current: Optional[float]) -> bool:
        prev_invalid = previous is None or not math.isfinite(previous)
        curr_invalid = current is None or not math.isfinite(current)
        if prev_invalid or curr_invalid:
            if prev_invalid and curr_invalid:
                return previous is None and current is not None
            return True
        return previous != current

    def _clear_op_log(self) -> None:
        if self.op_log_view is not None:
            self.op_log_view.clear()

    def _clear_debug_log(self) -> None:
        if self.debug_log_view is not None:
            self.debug_log_view.clear()

    def _clear_data_log(self) -> None:
        if self.data_view is not None:
            self.data_view.clear()

    def _set_metrics_history_mode(self, mode: str) -> None:
        if mode == "ONLINE":
            color = "#16a34a"
        else:
            mode = "OFFLINE"
            color = "#6b7280"
        self.metrics_history_mode_label.setText(mode)
        self.metrics_history_mode_label.setStyleSheet(
            f"background:{color}; color:#ffffff; padding:2px 8px; border-radius:4px;"
        )

    def _save_metrics_chart_history(self, device: Optional[DeviceInfo] = None) -> None:
        target = device or self.model.state.connected_device
        if target is None:
            return
        try:
            save_metrics_history(target.address, self.metrics_chart.export_points())
        except Exception as exc:
            self.on_log(f"Metrics history save failed: {exc}")

    def _load_metrics_chart_history(self, device: DeviceInfo) -> None:
        try:
            self.metrics_chart.set_online_mode()
            self._set_metrics_history_mode("ONLINE")
            self.metrics_chart.set_points(load_metrics_history(device.address))
        except Exception as exc:
            self.metrics_chart.clear()
            self.on_log(f"Metrics history load failed: {exc}")

    def _load_offline_metrics_chart_history(self, device: DeviceInfo) -> None:
        try:
            self.metrics_chart.set_offline_mode()
            self._set_metrics_history_mode("OFFLINE")
            self.metrics_chart.set_points(
                load_metrics_history_all(device.address),
                display_full_history=True,
            )
        except Exception as exc:
            self.metrics_chart.clear()
            self.on_log(f"Offline metrics history load failed: {exc}")

    @staticmethod
    def _snapshot_has_chart_data(snapshot: MetricsSnapshot) -> bool:
        values = (
            snapshot.fan_percent,
            snapshot.temperatures[3],
            snapshot.temperatures[2],
        )
        return any(value is not None and math.isfinite(value) for value in values)

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
        self._set_status_message(message)
        if self.debug_enabled and self.debug_log_view is not None:
            self.debug_log_view.append(self._format_log_entry(message))
        print(self._format_log_entry(message), flush=True)

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
            self.data_view.append(self._format_log_entry(f"Temp {channel + 1}: {text}"))

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
            self.data_view.append(self._format_log_entry(f"Fan {channel + 1}: {text}"))

    def _apply_fan_percent_value(self, value: Optional[float]) -> None:
        if self.fan_percent_field is None:
            return
        is_nc = value is None or not math.isfinite(value)
        if not is_nc and value is not None:
            value = min(100.0, max(0.0, value))
        text = "NC" if is_nc else f"{value:.0f}%"
        self.fan_percent_field.setText(text)

    def _apply_power_value(self, field, label: str, value: Optional[float], unit: str, decimals: int, append_data: bool) -> None:
        if field is None:
            return
        is_nc = value is None or not math.isfinite(value)
        text = "—" if is_nc else f"{value:.{decimals}f}"
        field.setText(text)
        if append_data:
            self.data_view.append(self._format_log_entry(f"{label}: {text} {unit}"))

    @Slot(object)
    def on_metrics_received(self, snapshot: MetricsSnapshot) -> None:
        prev_snapshot = self.metrics_snapshot
        self.metrics_snapshot = snapshot
        for channel, value in enumerate(snapshot.temperatures):
            self._apply_temp_value(
                channel,
                value,
                append_data=self._metric_value_changed(prev_snapshot.temperatures[channel], value),
            )
        for channel, value in enumerate(snapshot.fan_speeds):
            self._apply_fan_value(
                channel,
                value,
                append_data=self._metric_value_changed(prev_snapshot.fan_speeds[channel], value),
            )
        self._apply_fan_percent_value(snapshot.fan_percent)
        self.metrics_chart.add_sample(snapshot.fan_percent, snapshot.temperatures[3], snapshot.temperatures[2])
        limit = self.model.state.output_limit
        ina_unavailable = limit is not None and limit.state == OutputLimitState.SENSOR_ERROR
        self._apply_power_value(
            self.voltage_field,
            "Voltage",
            None if ina_unavailable else snapshot.voltage_v,
            "V",
            2,
            append_data=self._metric_value_changed(prev_snapshot.voltage_v, snapshot.voltage_v),
        )
        self._apply_power_value(
            self.current_field,
            "Current",
            None if ina_unavailable else snapshot.current_ma,
            "mA",
            1,
            append_data=self._metric_value_changed(prev_snapshot.current_ma, snapshot.current_ma),
        )
        self._refresh_temp_indicators()

    @Slot(object)
    def on_params_received(self, params: DeviceParams) -> None:
        saving_maximum = self.model.state.active_action == self.Action.SET_OUTPUT_MAX
        previous = self._device_params_snapshot
        self._set_params_fields(params, save=True, preserve_edits=saving_maximum)
        if saving_maximum and previous is not None and params.fan_min_speed < previous.fan_min_speed:
            self._show_apply_result("ok", f"Base fan speed reduced to {params.fan_min_speed}% to match the saved maximum.")
        if self.model.state.active_action == self.Action.DISCARD:
            self._finish_action(self.Action.DISCARD)

    @Slot(object, object, str)
    def on_params_command_done(self, command_id: object, status: Optional[ParamsStatus | ProfileApplyResult], error: str) -> None:
        action = self.model.handle_params_result(status is not None and status.ok, command_id)
        if action is None:
            return
        self._finish_action(action)
        if status is None:
            self._show_apply_result("error", error or "Error: apply failed")
            return
        if isinstance(status, ProfileApplyResult) and status.ok:
            reasons = {
                ProfileSkipReason.INVALID: "value is invalid or read-only",
                ProfileSkipReason.OUTPUT_MAX: "above the global maximum output",
                ProfileSkipReason.TEMPERATURE_CONFLICT: "temperature thresholds conflict",
                ProfileSkipReason.ADC_UNAVAILABLE: "ADS1115 unavailable",
                ProfileSkipReason.NTC_UNAVAILABLE: "control thermistor unavailable",
                ProfileSkipReason.OUTPUT_ADJUSTING: "overload protection is adjusting the output",
            }
            skipped = [
                f"{PARAM_LABELS_BY_ID.get(field, field)}: {reasons[reason]}"
                for field, reason in enumerate(status.skipped)
                if reason != ProfileSkipReason.NONE and not status.applied_mask & (1 << field)
            ]
            capped = [
                f"{PARAM_LABELS_BY_ID.get(field, field)}: reduced to the saved maximum output"
                for field, reason in enumerate(status.skipped)
                if reason == ProfileSkipReason.OUTPUT_MAX and status.applied_mask & (1 << field)
            ]
            title = (
                "No profile fields applied" if status.applied_mask == 0
                else "Profile applied partially" if skipped else "Profile applied"
            )
            message = f"Profile {status.slot}: {title}"
            if skipped or capped:
                message += " (" + "; ".join(capped + skipped) + ")"
            self.on_log(message)
            self._show_apply_result("ok", message)
            self.worker.submit(self.worker.read_params_snapshot())
            return
        if status.ok:
            self.on_log("Parameters applied")
            self._show_apply_result("ok", "Applied")
            save_slot = self.model.state.command_slot
            if self.model.state.active_action == self.Action.SAVE_PROFILE:
                self._submit_profile_action(lambda: self.worker.save_profile(save_slot))
            self.worker.submit(self.worker.read_params_snapshot())
            return
        if status.status == PARAM_STATUS_OUTPUT_ADJUSTING:
            message = "Settings were not saved. Wait for overload protection to finish, then apply again."
            self.on_log(message)
        elif status.status == self.PARAM_STATUS_BUSY:
            if isinstance(status, ParamsStatus) and status.field_id == PARAM_FIELD_FAN_MIN_SPEED:
                message = "Cannot change minimum output while overload protection is adjusting"
            else:
                message = "Error: device busy"
            self.on_log(message)
        elif status.status == PARAM_STATUS_ADC_UNAVAILABLE:
            message = "Cannot enable temperature mode: ADS1115 unavailable"
            self.on_log(message)
        elif status.status == PARAM_STATUS_NTC_UNAVAILABLE:
            message = "Cannot enable temperature mode: control thermistor unavailable"
            self.on_log(message)
        elif status.status == PARAM_STATUS_STORAGE_ERROR:
            message = "Error: could not commit parameters to device storage"
            self.on_log("Parameters not applied: device storage commit failed")
        else:
            field_id = status.field_id if isinstance(status, ParamsStatus) else None
            field_label = (
                PARAM_LABELS_BY_ID.get(field_id, "unknown field")
                if field_id is not None
                else "unknown field"
            )
            detail = PARAM_ERROR_MESSAGES.get(field_id, field_label)
            message = f"Error: {detail}"
            self.on_log(f"Parameter error: {detail}")
        self._show_apply_result("error", message)

    @Slot(object, object)
    def on_fan_mode_status(self, status: FanModeStatus, connection_id: object) -> None:
        if self.model.handle_fan_mode_status(status, connection_id):
            self._apply_ui()

    @Slot(object, object)
    def on_output_limit(self, status, connection_id: object) -> None:
        previous = self.model.state.output_limit
        if self.model.handle_output_limit(status, connection_id):
            if status.state == OutputLimitState.SENSOR_ERROR:
                self._apply_power_value(self.voltage_field, "Voltage", None, "V", 2, append_data=False)
                self._apply_power_value(self.current_field, "Current", None, "mA", 1, append_data=False)
            if previous is None or previous.configured != status.configured:
                self.output_max_input.setValue(status.configured)
            self._apply_ui()

    @Slot(object, str)
    def on_output_limit_command_done(self, command_id: object, error: str) -> None:
        if self.model.state.command_id is not command_id:
            return
        action = self.model.state.active_action
        if action not in (self.Action.SET_OUTPUT_MAX, self.Action.RESET_OUTPUT_LIMIT):
            return
        self._finish_action(action)
        if error:
            self.on_log(f"Output-limit command failed: {error}")
            self._show_apply_result("error", error)

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
        self.ina_warning_label.setVisible(bool(status.error_mask & DEVICE_ERROR_INA_OFFLINE))
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

    @Slot(int, str)
    def on_operation_start_failed(self, op_type: int, error: str) -> None:
        action = self.model.handle_operation_start_failure(op_type)
        if action is None:
            return
        op_label = OP_TYPE_NAMES.get(op_type, f"OP{op_type}")
        self.on_log(f"Could not start {op_label}: {error}")
        self._finish_action(action)

    @Slot(object, object)
    def on_operation_status(self, status: OperationStatus, connection_id: object) -> None:
        previous = self.model.state.operation_status
        completed = self.model.handle_operation_status(status, connection_id)
        if (self.model.state.operation_status is not status
                or self.model.state.connection_id is not connection_id):
            return
        op_label = OP_TYPE_NAMES.get(status.op_type, f"OP{status.op_type}")
        state_label = OP_STATE_NAMES.get(status.state, "UNKNOWN")
        started = operation_running(status) and (
            not operation_running(previous) or previous.op_type != status.op_type
        )
        if started:
            self._clear_op_log()
        if self.op_log_view is not None and not (
            status.op_type == OP_TYPE_NONE and status.state == OP_STATE_IDLE
        ):
            self.op_log_view.append(self._format_log_entry(f"{op_label}: {status.error or state_label}"))
        if completed is not None:
            self._finish_action(completed)
        if started:
            self._set_status_message(f"Operation started: {op_label}")
        elif status.state == OP_STATE_DONE:
            self._set_status_message(f"Operation completed: {op_label}")
            self.worker.submit(self.worker.read_params_snapshot())
        elif status.state == OP_STATE_ERROR:
            self._set_status_message(f"Operation error {op_label}: {status.error or 'unknown error'}")
        self._apply_ui()

    @Slot(int, int, bool, str)
    def on_profile_action_done(self, action: int, slot: int, ok: bool, error: str) -> None:
        command = PROFILE_COMMANDS.get(action)
        if command is None or not self.model.handle_profile_result(command, slot):
            return
        self._finish_action(command)
        if not ok:
            self.on_log(error)
            self._show_apply_result("error", error)
        elif action == PROFILE_ACTION_SAVE_CURRENT:
            self.on_log(f"Saved profile {slot} on controller")
        elif action == PROFILE_ACTION_CLEAR:
            self._reset_profile_selection()
            self.on_log(f"Cleared profile {slot} on controller")
        self._refresh_profile_buttons()

    @Slot(int, object, bool, str)
    def on_profile_metadata_done(
        self, slot: int, metadata: object, ok: bool, error: str
    ) -> None:
        if not self.model.handle_profile_result(self.Action.EDIT_PROFILE, slot):
            return
        self._finish_action(self.Action.EDIT_PROFILE)
        if ok:
            self._device_profile_metadata[slot] = dict(metadata)
            self.on_log(f"Updated profile {slot} metadata on controller")
        else:
            self.on_log(error)
        self._refresh_profile_buttons()

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

    @Slot(object)
    def on_connection_event(self, event: ConnEvent) -> None:
        previous = self.model.state
        if not self.model.handle_connection_event(event):
            return
        if previous.active_action is not None and previous.active_action != self.model.state.active_action:
            if event.kind == ConnEventKind.LINK_CLOSED:
                future = self._action_futures.get(previous.active_action)
                if future is not None:
                    future.cancel()
            self._finish_action(previous.active_action)

        if event.kind in (ConnEventKind.CONNECT_REQUEST, ConnEventKind.OLD_SESSION_CLOSED):
            self._apply_ui()
            return
        if event.kind == ConnEventKind.AUTO_STOPPED and previous.conn == self.ConnState.DISCONNECTED:
            self._apply_ui()
            return
        if event.kind == ConnEventKind.CONNECT_SUCCEEDED:
            device = event.device
            assert device is not None
            self._startup_auto_connect_device = None
            self._reset_profiles_for_device()
            self.worker.submit(self.worker.read_profiles())
            if self._device_params_snapshot is None:
                self._set_params_fields(load_device_params(device.address), save=False)
            update_paired_last_connected(device.address)
            self._load_metrics_chart_history(device)
            if self._snapshot_has_chart_data(self.metrics_snapshot):
                self.metrics_chart.add_sample(
                    self.metrics_snapshot.fan_percent,
                    self.metrics_snapshot.temperatures[3],
                    self.metrics_snapshot.temperatures[2],
                )
            self._lock_selection_to_connected()
            self._apply_ui()
            self.on_log(f"Connected to {device.name}")
            return
        disconnected_device = previous.connected_device
        self._save_metrics_chart_history(disconnected_device)
        self._device_profile_slots = set()
        self._device_profile_metadata = {}
        self._reset_profile_selection()
        self._refresh_profile_buttons()
        self._clear_paired_selection()
        self._device_params_snapshot = None
        self._reset_temp_fields()
        self._reset_params_fields()
        if disconnected_device is not None:
            self._load_offline_metrics_chart_history(disconnected_device)
        else:
            self.metrics_chart.set_offline_mode()
            self._set_metrics_history_mode("OFFLINE")
        self._apply_ui()
        startup_auto_failed = self._startup_auto_connect_device is not None
        failed_device = self._startup_auto_connect_device
        self._startup_auto_connect_device = None
        if startup_auto_failed and failed_device is not None:
            self.on_log(f"Auto-connect failed: {failed_device.name}")
            return
        if disconnected_device is not None:
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
        if self.fan_percent_field is not None:
            self.fan_percent_field.setText("—")
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
            self.ina_warning_label.hide()
        self._set_device_status_indicator("#6b7280")
        self.metrics_chart.clear()
        self.metrics_chart.set_offline_mode()
        self._set_metrics_history_mode("OFFLINE")
