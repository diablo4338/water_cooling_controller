from __future__ import annotations

import math

from PySide6.QtWidgets import QInputDialog, QMenu

from ..core import (
    DeviceParams,
    FAN_CONTROL_DC,
    FAN_MODE_CONTINUOUS,
    clear_device_profile,
    load_device_params,
    load_device_profile_metadata,
    load_device_profiles,
    save_device_params,
    save_device_profile,
    save_device_profile_metadata,
)
from .constants import FAN_MONITORING_KEYS, PARAM_FIELDS


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
        return self._default_device_params()

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
        params = self._default_device_params()
        self._device_params_snapshot = None
        self._params_update_lock = True
        try:
            for item in self.param_fields:
                spec = item["spec"]
                widget = item["widget"]
                value = getattr(params, spec["key"])
                if spec["kind"] == "enum":
                    idx = widget.findData(int(value))
                    widget.setCurrentIndex(idx if idx >= 0 else 0)
                elif spec["kind"] == "bool":
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
        self,
        params: DeviceParams,
        save: bool = True,
        apply_status: str | None = None,
        apply_status_text: str | None = None,
    ) -> None:
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
                elif spec["kind"] == "bool":
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
                self.fan_monitor_checkboxes[idx].setChecked(bool(getattr(params, key)))
                self.fan_monitor_checkboxes[idx].setEnabled(idx > 0)
        finally:
            self._params_update_lock = False
        if save and self.model.state.connected_device:
            from .. import gui as gui_module

            gui_module.save_device_params(self.model.state.connected_device.address, params)
        if apply_status is not None and apply_status_text is not None:
            self._set_apply_status(apply_status, apply_status_text)
        self._refresh_temp_indicators()

    def _current_params(self) -> DeviceParams:
        if len(self.param_fields) != len(PARAM_FIELDS) or len(self.fan_monitor_checkboxes) != 4:
            device = self.model.state.connected_device
            return load_device_params(device.address) if device else self._device_params_for_metrics()
        values: dict[str, object] = {}
        for item in self.param_fields:
            spec = item["spec"]
            widget = item["widget"]
            if spec["kind"] == "enum":
                value = int(widget.currentData())
            elif spec["kind"] == "bool":
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

    def _refresh_profile_buttons(self) -> None:
        for index, button in enumerate(self.profile_buttons, start=1):
            exists = index in self._device_profiles
            metadata = self._device_profile_metadata.get(index, {})
            button.setText(metadata.get("title", f"P{index}"))
            button.setToolTip(
                metadata.get("description")
                or (
                    "Saved profile. Click to apply."
                    if exists
                    else "Empty profile. Select it, adjust settings, then press Apply to save."
                )
            )
            status_bar = self.profile_status_bars[index - 1]
            color = "#22c55e" if exists else "#9ca3af"
            status_bar.setStyleSheet(f"background:{color};")
            status_bar.setToolTip("Profile has saved settings" if exists else "Profile is empty")
        connected = self.model.state.connected_device is not None
        for button in self.profile_buttons:
            button.setEnabled(connected and not self.model.state.busy)
        selected_profile_is_saved = (
            self._selected_profile_index in self._device_profiles
            if self._selected_profile_index is not None
            else False
        )
        self.clear_profile_button.setEnabled(
            connected and not self.model.state.busy and selected_profile_is_saved
        )

    def _reset_profile_selection(self) -> None:
        self._selected_profile_index = None
        for button in self.profile_buttons:
            button.setChecked(False)

    def _load_profiles_for_device(self, address: str) -> None:
        try:
            self._device_profiles = load_device_profiles(address)
            self._device_profile_metadata = load_device_profile_metadata(address)
        except (OSError, ValueError) as exc:
            self._device_profiles = {}
            self._device_profile_metadata = {}
            self.on_log(f"Could not load profiles: {exc}")
        self._reset_profile_selection()
        self._refresh_profile_buttons()

    def on_profile_clicked(self, index: int, checked: bool) -> None:
        device = self.model.state.connected_device
        if device is None or self.model.state.conn != self.ConnState.CONNECTED:
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
        params = self._device_profiles.get(index)
        if params is None:
            self.model.set_status(
                f"Profile {index} selected. Adjust settings and press Apply to save it."
            )
            self._apply_ui()
            return
        self._set_params_fields(params, save=False)
        self.on_apply()

    def _save_profile_metadata(self, index: int, title: str, description: str) -> None:
        device = self.model.state.connected_device
        if device is None:
            return
        try:
            save_device_profile_metadata(device.address, index, title, description)
        except (OSError, ValueError) as exc:
            self.on_log(f"Could not update profile {index}: {exc}")
            return
        self._device_profile_metadata[index] = {
            "title": title.strip(),
            "description": description.strip(),
        }
        self._refresh_profile_buttons()

    def _show_profile_menu(self, index: int, global_position) -> None:
        if self.model.state.connected_device is None or self.model.state.busy:
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

    def on_clear_profile(self) -> None:
        index = self._selected_profile_index
        device = self.model.state.connected_device
        if index is None or device is None or index not in self._device_profiles:
            return
        try:
            clear_device_profile(device.address, index)
        except (OSError, ValueError) as exc:
            self.on_log(f"Could not clear profile {index}: {exc}")
            return
        del self._device_profiles[index]
        self._reset_profile_selection()
        self._refresh_profile_buttons()
        self.on_log(f"Cleared profile {index}")

    def _on_params_changed(self, _) -> None:
        if self._params_update_lock:
            return
        device = self.model.state.connected_device
        if device is None or self.model.state.conn != self.ConnState.CONNECTED:
            return
        self.model.set_status("Parameters changed. Press Apply to write them.")
        self._apply_ui()
        self._refresh_temp_indicators()

    def _set_apply_status(self, kind: str, text: str) -> None:
        self._apply_status_kind = kind
        self._apply_status_text = text
        color = {
            "idle": "#6b7280",
            "ok": "#16a34a",
            "error": "#dc2626",
        }.get(kind, "#6b7280")
        if self.apply_status_label is not None:
            self.apply_status_label.setStyleSheet(f"color:{color};")
            self.apply_status_label.setText(text)
            self.apply_status_label.setToolTip(text)

    def _show_apply_result(self, kind: str, text: str) -> None:
        self._set_apply_status(kind, text)
        if hasattr(self, "_apply_status_timer") and self._apply_status_timer is not None:
            self._apply_status_timer.stop()
        self._apply_status_timer.start(5000)
        self._apply_ui()

    def _clear_apply_status(self) -> None:
        if hasattr(self, "_apply_status_timer") and self._apply_status_timer is not None:
            self._apply_status_timer.stop()
        self._set_apply_status("idle", "")
