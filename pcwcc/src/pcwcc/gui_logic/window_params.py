from __future__ import annotations

from typing import TYPE_CHECKING

import math
from collections.abc import Callable, Coroutine, Iterable, Mapping
from dataclasses import fields, replace

from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QCheckBox, QInputDialog, QMenu

from ..core import (
    DeviceParams,
    FAN_CONTROL_DC,
    FAN_MODE_CONTINUOUS,
    load_device_params,
)
from ..ui.theme import describe, set_visual
from .constants import FAN_MONITORING_KEYS, PARAM_FIELDS


if TYPE_CHECKING:
    from .app import MainWindow


class MainWindowParamsMixin:
    @staticmethod
    def _default_device_params() -> DeviceParams:
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
            overheat_alarm_enabled=True,
            protocol_version=6,
        )

    def _set_fan_status_indicator(self: MainWindow, channel: int, severity: str) -> None:
        if channel < len(self.fan_status_indicators):
            set_visual(self.fan_status_indicators[channel], "fluentSeverity", severity)

    def _set_fan_status_tooltip(self: MainWindow, channel: int, text: str) -> None:
        if channel < len(self.fan_status_indicators):
            describe(self.fan_status_indicators[channel], text)

    def _set_device_status_indicator(self: MainWindow, severity: str) -> None:
        if self.device_status_indicator is not None:
            set_visual(self.device_status_indicator, "fluentSeverity", severity)

    def _set_temp_indicator(self: MainWindow, channel: int, severity: str, text: str) -> None:
        if channel < len(self.temp_indicators):
            set_visual(self.temp_indicators[channel], "fluentSeverity", severity)
            describe(self.temp_indicators[channel], text)

    def _device_params_for_metrics(self: MainWindow) -> DeviceParams:
        if self._device_params_snapshot is not None:
            return self._device_params_snapshot
        device = self.model.state.connected_device
        if device:
            return load_device_params(device.address)
        return self._default_device_params()

    def _refresh_temp_indicators(self: MainWindow) -> None:
        max_temp = float(self._device_params_for_metrics().fan_max_temp)
        for channel, value in enumerate(self.temp_values):
            if value is None or not math.isfinite(value):
                self._set_temp_indicator(channel, "neutral", "NC")
                continue
            delta = max_temp - value
            if delta > 5.0:
                self._set_temp_indicator(channel, "success", f"{delta:.2f}C below max")
            elif value < max_temp:
                self._set_temp_indicator(channel, "warning", f"{delta:.2f}C below max")
            else:
                self._set_temp_indicator(channel, "danger", "At or above max")

    def _reset_params_fields(self: MainWindow) -> None:
        params = self._default_device_params()
        self._device_params_snapshot = None
        self._params_update_lock = True
        try:
            for item in self.param_fields:
                spec = item["spec"]
                widget = item["widget"]
                value = getattr(params, spec["key"])
                if isinstance(widget, QCheckBox):
                    widget.setChecked(bool(value))
                else:
                    widget.setValue(value)
                widget.setEnabled(False)
            for idx, key in enumerate(FAN_MONITORING_KEYS):
                self.fan_monitor_checkboxes[idx].setChecked(bool(getattr(params, key)))
                self.fan_monitor_checkboxes[idx].setEnabled(False)
        finally:
            self._params_update_lock = False
        self._clear_apply_status()

    def _set_params_fields(
        self: MainWindow,
        params: DeviceParams,
        save: bool = True,
        apply_status: str | None = None,
        apply_status_text: str | None = None,
        preserve_edits: bool = False,
    ) -> None:
        if len(self.param_fields) != len(PARAM_FIELDS) or len(self.fan_monitor_checkboxes) != 4:
            return
        displayed = params
        previous = self._device_params_snapshot
        if preserve_edits and previous is not None:
            draft = self._current_params()
            edits = {
                field.name: getattr(draft, field.name)
                for field in fields(DeviceParams)
                if getattr(draft, field.name) != getattr(previous, field.name)
            }
            displayed = replace(params, **edits)
        self._device_params_snapshot = params
        self._params_update_lock = True
        try:
            for item in self.param_fields:
                spec = item["spec"]
                widget = item["widget"]
                value = getattr(displayed, spec["key"])
                if isinstance(widget, QCheckBox):
                    widget.setChecked(bool(value))
                else:
                    widget.setValue(value)
                widget.setEnabled(True)
                if spec["key"] == "overheat_alarm_enabled" and params.protocol_version < 6:
                    widget.setEnabled(False)
                    widget.setToolTip("Requires firmware with parameters protocol v6")
                elif spec["key"] == "overheat_alarm_enabled":
                    widget.setToolTip("")
            for idx, key in enumerate(FAN_MONITORING_KEYS):
                self.fan_monitor_checkboxes[idx].setChecked(bool(getattr(displayed, key)))
                self.fan_monitor_checkboxes[idx].setEnabled(idx > 0)
        finally:
            self._params_update_lock = False
        if save and self.model.state.connected_device:
            from .. import gui as gui_module

            gui_module.save_device_params(self.model.state.connected_device.address, params)
        if apply_status is not None and apply_status_text is not None:
            self._set_apply_status(apply_status, apply_status_text)
        self._refresh_temp_indicators()

    def _current_params(self: MainWindow) -> DeviceParams:
        if len(self.param_fields) != len(PARAM_FIELDS) or len(self.fan_monitor_checkboxes) != 4:
            device = self.model.state.connected_device
            return load_device_params(device.address) if device else self._device_params_for_metrics()
        values: dict[str, int | bool] = {}
        for item in self.param_fields:
            spec = item["spec"]
            widget = item["widget"]
            if isinstance(widget, QCheckBox):
                value = widget.isChecked()
            else:
                value = int(widget.value())
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
            overheat_alarm_enabled=bool(values["overheat_alarm_enabled"]),
            protocol_version=(
                self._device_params_snapshot.protocol_version
                if self._device_params_snapshot is not None
                else 6
            ),
        )

    def _refresh_profile_buttons(self: MainWindow) -> None:
        for index, button in enumerate(self.profile_buttons, start=1):
            exists = index in self._device_profile_slots
            metadata = self._device_profile_metadata.get(index, {})
            button.setText(metadata.get("title") or f"P{index}")
            button.setToolTip(
                metadata.get("description")
                or (
                    "Saved profile. Click to apply."
                    if exists
                    else "Empty profile. Select it, adjust settings, then press Apply to save."
                )
            )
            status_bar = self.profile_status_bars[index - 1]
            severity = "success" if exists else "neutral"
            set_visual(status_bar, "fluentSeverity", severity)
            status_bar.setToolTip("Profile has saved settings" if exists else "Profile is empty")
        self._apply_ui()

    def _reset_profile_selection(self: MainWindow) -> None:
        self._selected_profile_index = None
        for button in self.profile_buttons:
            button.setChecked(False)

    def _reset_profiles_for_device(self: MainWindow) -> None:
        self._device_profile_metadata = {}
        self._device_profile_slots = set()
        self._reset_profile_selection()
        self._refresh_profile_buttons()

    def on_profiles_received(self: MainWindow, slots: Iterable[int]) -> None:
        self._device_profile_slots = set(slots)
        self._refresh_profile_buttons()

    def on_profile_metadata_received(self: MainWindow, metadata: Mapping[int, dict[str, str]]) -> None:
        self._device_profile_metadata = dict(metadata)
        self._refresh_profile_buttons()

    def _submit_profile_action(
        self: MainWindow, action: Callable[[], Coroutine[object, object, None]]
    ) -> None:
        active_action = self.model.state.active_action
        if active_action is not None:
            self._start_action(active_action, action())

    def on_profile_clicked(self: MainWindow, index: int, checked: bool) -> None:
        if self.Action.APPLY_PROFILE not in self.model.ui.enabled_actions:
            return
        if not checked:
            if self._selected_profile_index == index:
                self._selected_profile_index = None
                self._apply_ui()
            return
        for other_index, button in enumerate(self.profile_buttons, start=1):
            if other_index != index:
                button.setChecked(False)
        self._selected_profile_index = index
        if index not in self._device_profile_slots:
            self.model.set_status(
                f"Profile {index} selected. Adjust settings and press Apply to save it."
            )
            self._apply_ui()
            return
        if self.model.dispatch(self.Action.APPLY_PROFILE, slot=index) is None:
            return
        self._apply_ui()
        self._submit_profile_action(lambda: self.worker.apply_profile(index, self.model.state.command_id))

    def _save_profile_metadata(self: MainWindow, index: int, title: str, description: str) -> None:
        if self.model.dispatch(self.Action.EDIT_PROFILE, slot=index) is None:
            return
        self._apply_ui()
        self._submit_profile_action(
            lambda: self.worker.save_profile_metadata(index, title.strip(), description.strip())
        )

    def _show_profile_menu(self: MainWindow, index: int, global_position: QPoint) -> None:
        if self.Action.EDIT_PROFILE not in self.model.ui.enabled_actions:
            return
        metadata = self._device_profile_metadata.get(index, {})
        menu = QMenu(self)
        rename_action = menu.addAction("Rename profile...")
        description_action = menu.addAction("Edit description...")
        selected_action = menu.exec(global_position)
        if selected_action == rename_action:
            title, accepted = QInputDialog.getText(
                self,
                "Rename profile",
                "Profile title:",
                text=metadata.get("title", f"P{index}"),
            )
            if accepted:
                normalized_title = title.strip()
                if not normalized_title:
                    self.on_log("Profile title cannot be empty")
                    return
                self._save_profile_metadata(
                    index,
                    normalized_title,
                    metadata.get("description", ""),
                )
        elif selected_action == description_action:
            description, accepted = QInputDialog.getMultiLineText(
                self,
                "Edit profile description",
                "Description shown in the profile tooltip:",
                metadata.get("description", ""),
            )
            if accepted:
                self._save_profile_metadata(
                    index,
                    metadata.get("title", f"P{index}"),
                    description,
                )

    def on_clear_profile(self: MainWindow) -> None:
        index = self._selected_profile_index
        if index not in self._device_profile_slots:
            return
        if self.model.dispatch(self.Action.CLEAR_PROFILE, slot=index) is None:
            return
        self._apply_ui()
        self._submit_profile_action(lambda: self.worker.clear_profile(index))

    def _on_params_changed(self: MainWindow, _: int) -> None:
        if self._params_update_lock:
            return
        device = self.model.state.connected_device
        if device is None or self.model.state.conn != self.ConnState.CONNECTED:
            return
        self.model.set_status("Parameters changed. Press Apply to write them.")
        self._apply_ui()
        self._refresh_temp_indicators()

    def _set_apply_status(self: MainWindow, kind: str, text: str) -> None:
        self._apply_status_kind = kind
        self._apply_status_text = text
        severity = {
            "idle": "neutral",
            "ok": "success",
            "error": "danger",
        }.get(kind, "neutral")
        if self.apply_status_label is not None:
            set_visual(self.apply_status_label, "fluentSeverity", severity)
            self.apply_status_label.setToolTip(text)
            self.apply_status_message.set_message(text)

    def _show_apply_result(self: MainWindow, kind: str, text: str) -> None:
        self._set_apply_status(kind, text)
        if hasattr(self, "_apply_status_timer") and self._apply_status_timer is not None:
            self._apply_status_timer.stop()
        self._apply_status_timer.start(5000)
        self._apply_ui()

    def _clear_apply_status(self: MainWindow) -> None:
        if hasattr(self, "_apply_status_timer") and self._apply_status_timer is not None:
            self._apply_status_timer.stop()
        self._set_apply_status("idle", "")
