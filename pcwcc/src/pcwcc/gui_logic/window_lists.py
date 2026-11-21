from __future__ import annotations

from typing import TYPE_CHECKING, cast

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush
from PySide6.QtWidgets import QInputDialog, QListWidget, QListWidgetItem

from ..core import (
    DeviceInfo,
    FAN_MODE_NAMES,
    add_or_update_paired,
    get_auto_connect_address,
    load_paired_records,
    rename_paired_record,
    set_paired_auto_connect,
    save_paired_records,
)
from ..core_logic.models import OutputLimitState, OutputLimitStatus, ThermalInputState
from ..presentation import SelectionSource
from ..ui.theme import theme_color
from .constants import USER_ROLE


if TYPE_CHECKING:
    from .app import MainWindow


class MainWindowListMixin:
    def _update_output_max_save(self: MainWindow) -> None:
        self.output_max_save.setEnabled(self.output_max_input.isEnabled() and self.output_max_input.value() > 0)

    def _apply_ui(self: MainWindow) -> None:
        ui = self.model.ui
        status = ui.status_text
        if self.model.state.connected_device:
            status = f"{status} | {self.model.state.connected_device.name}"
        self.status_label.setText(status)
        self.status_label.setToolTip(status)

        for widget, action in (
            (self.scan_button, self.Action.SCAN),
            (self.pair_button, self.Action.PAIR),
            (self.connect_button, self.Action.CONNECT),
            (self.disconnect_button, self.Action.DISCONNECT),
            (self.apply_button, self.Action.APPLY),
            (self.discard_button, self.Action.DISCARD),
            (self.calibrate_button, self.Action.CALIBRATE),
            (self.setup_fans_button, self.Action.SETUP_FANS),
            (self.delete_button, self.Action.DELETE_PAIRED),
            (self.auto_checkbox, self.Action.AUTO_CONNECT),
        ):
            widget.setEnabled(action in ui.enabled_actions)

        can_rename_paired = (
            self.model.state.conn == self.ConnState.DISCONNECTED
            and not self.model.state.busy
            and self.model.state.selected_source == self.SelectionSource.PAIRED
            and self.model.state.selected_device is not None
            and not self.model.state.auto_enabled
        )
        self.rename_button.setEnabled(can_rename_paired)

        mode = self.model.state.fan_mode_status
        mode_message = ""
        if mode is not None and mode.selected != mode.effective:
            effective = "Service operation" if mode.effective == 3 else FAN_MODE_NAMES[mode.effective]
            reason = {
                ThermalInputState.ADC_INITIALIZING: "waiting for ADS1115 readings",
                ThermalInputState.ADC_OFFLINE: "ADS1115 unavailable: I\u00b2C error or recovery",
                ThermalInputState.NTC_UNAVAILABLE: "control thermistor unavailable: no valid temperature",
            }.get(mode.thermal_input)
            detail = f"selected: {FAN_MODE_NAMES[mode.selected]}"
            if reason:
                detail += f"; {reason}"
            mode_message = f"Current fan mode: {effective} ({detail})"
        self.effective_fan_mode_message.set_message(mode_message)

        limit = self.model.state.output_limit
        self.output_max_input.setEnabled(self.Action.SET_OUTPUT_MAX in ui.enabled_actions)
        self._update_output_max_save()
        self.output_limit_reset.setEnabled(
            self.Action.RESET_OUTPUT_LIMIT in ui.enabled_actions
            and limit is not None and limit.reset_sequence is not None
        )
        if self.model.state.conn == self.ConnState.DISCONNECTED:
            self.output_max_input.setValue(0)
        limit_warning = self._output_limit_warning(limit)
        self.output_limit_message.set_message(limit_warning)

        params_enabled = ui.can_edit_params and self._device_params_snapshot is not None
        for button in self.profile_buttons:
            button.setEnabled(self.Action.APPLY_PROFILE in ui.enabled_actions)
        self.clear_profile_button.setEnabled(
            self.Action.CLEAR_PROFILE in ui.enabled_actions
            and self._selected_profile_index in self._device_profile_slots
        )
        for item in self.param_fields:
            requires_v6 = (
                item["spec"]["key"] == "overheat_alarm_enabled"
                and self._device_params_snapshot is not None
                and self._device_params_snapshot.protocol_version < 6
            )
            item["widget"].setEnabled(params_enabled and not requires_v6)
        for idx, checkbox in enumerate(self.fan_monitor_checkboxes):
            checkbox.setEnabled(params_enabled and idx > 0)

        self.auto_checkbox.blockSignals(True)
        self.auto_checkbox.setChecked(ui.auto_enabled)
        self.auto_checkbox.blockSignals(False)
        self.auto_checkbox.setVisible(self.debug_enabled)
        self._apply_paired_highlight()

    @staticmethod
    def _output_limit_warning(limit: OutputLimitStatus | None) -> str:
        if limit is None:
            return ""
        if limit.configured == 0:
            return "Output blocked: saved limit unavailable. Set and save the maximum in Settings."
        if limit.effective == limit.configured:
            return ""
        details = {
            OutputLimitState.CONFIRMING: "confirming overload",
            OutputLimitState.START_BLOCKED: "start blocked by overload protection",
            OutputLimitState.VERIFY_TIMEOUT: "could not verify the limit after the output changed",
            OutputLimitState.MEASURE: "measuring overload",
            OutputLimitState.VERIFY_FORMULA: "checking calculated limit",
            OutputLimitState.VERIFY_STEP: "checking a correction step",
            OutputLimitState.VERIFY_MINIMUM: "overload: checking minimum",
            OutputLimitState.LIMITED: "output adjusted due to load",
            OutputLimitState.MINIMUM: "limited to minimum while INA226 is unavailable",
            OutputLimitState.SENSOR_ERROR: "INA226 unavailable during overload; applying the fallback limit",
            OutputLimitState.FAULT: "overload alarm; output shut off",
        }.get(limit.state)
        if limit.alert_without_load:
            details = "abnormal overload with no load"
        effective = f"{limit.effective}%"
        if limit.temporary_minimum is not None and limit.state == OutputLimitState.LIMITED:
            effective = f"{limit.temporary_minimum}\u2013{effective}"
        message = f"Output limit reduced to {effective} (saved maximum {limit.configured}%)."
        return f"{message} {details.capitalize()}." if details else message

    def _apply_paired_highlight(self: MainWindow) -> None:
        highlight = self.model.ui.paired_highlight
        for idx in range(self.paired_list.count()):
            item = self.paired_list.item(idx)
            info = item.data(USER_ROLE)
            if highlight and info and getattr(info, "address", None) == highlight.address:
                item.setBackground(QBrush(theme_color("success_background")))
            else:
                item.setBackground(QBrush())

    def _clear_paired_selection(self: MainWindow) -> None:
        self.paired_list.blockSignals(True)
        self.paired_list.clearSelection()
        self.paired_list.blockSignals(False)
        if self.model.state.selected_source == self.SelectionSource.PAIRED:
            self.model.set_selection(None, None)

    def _lock_selection_to_connected(self: MainWindow) -> None:
        if self._lock_selection_active:
            return
        device = self.model.state.connected_device
        if not device:
            return
        self._lock_selection_active = True
        try:
            self.found_list.blockSignals(True)
            self.found_list.clearSelection()
            self.found_list.blockSignals(False)
            target_item = None
            for idx in range(self.paired_list.count()):
                item = self.paired_list.item(idx)
                info = item.data(USER_ROLE)
                if info and getattr(info, "address", None) == device.address:
                    target_item = item
                    break
            self.paired_list.blockSignals(True)
            if target_item is not None:
                self.paired_list.setCurrentItem(target_item)
                self.paired_list.scrollToItem(target_item)
            else:
                self.paired_list.clearSelection()
            self.paired_list.blockSignals(False)
            self.model.set_selection(self.SelectionSource.PAIRED if target_item else None, device if target_item else None)
        finally:
            self._lock_selection_active = False

    def _on_found_selected(self: MainWindow) -> None:
        self._sync_selection(self.SelectionSource.FOUND, self.found_list, self.paired_list)

    def _on_paired_selected(self: MainWindow) -> None:
        self._sync_selection(self.SelectionSource.PAIRED, self.paired_list, self.found_list)

    def _sync_selection(self: MainWindow, source: SelectionSource, current_list: QListWidget, other_list: QListWidget) -> None:
        if self.model.state.conn == self.ConnState.CONNECTED:
            self._lock_selection_to_connected()
            self._apply_ui()
            return
        item = cast(QListWidgetItem | None, current_list.currentItem())
        if item is not None:
            other_list.blockSignals(True)
            other_list.clearSelection()
            other_list.blockSignals(False)
            device = item.data(USER_ROLE)
            self.model.set_selection(source, device)
            if source == self.SelectionSource.PAIRED and device is not None:
                self._load_offline_metrics_chart_history(device)
        else:
            self.model.set_selection(None, None)
        self._apply_ui()

    @staticmethod
    def _load_paired() -> list[DeviceInfo]:
        devices = []
        for item in load_paired_records():
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=item.get("name", "Unknown"), address=address))
        return devices

    @staticmethod
    def _auto_connect_address() -> str | None:
        return get_auto_connect_address()

    @staticmethod
    def _remove_paired(address: str) -> bool:
        raw = load_paired_records()
        new_raw = [item for item in raw if item.get("address") != address]
        if len(new_raw) == len(raw):
            return False
        save_paired_records(new_raw)
        return True

    @staticmethod
    def _add_paired(device: DeviceInfo, k_hex: str) -> None:
        add_or_update_paired(device, k_hex)

    def _refresh_paired_list(self: MainWindow) -> None:
        auto_connect_address = self._auto_connect_address()
        self.paired_list.blockSignals(True)
        self.paired_list.clear()
        devices = self._load_paired()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(USER_ROLE, dev)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(
                Qt.CheckState.Checked
                if auto_connect_address == dev.address
                else Qt.CheckState.Unchecked
            )
            self.paired_list.addItem(item)
        self.paired_list.blockSignals(False)
        self.model.set_paired_devices(devices)
        if self.model.state.conn == self.ConnState.CONNECTED:
            self._lock_selection_to_connected()
            self._apply_ui()
            return
        state = self.model.state
        if state.selected_source == self.SelectionSource.PAIRED:
            previous = state.selected_device
            if previous is not None and previous.address in {device.address for device in devices}:
                self._select_paired_device(previous)
            else:
                self.model.set_selection(None, None)
                self.paired_list.clearSelection()
        self._apply_ui()

    def _on_paired_item_changed(self: MainWindow, item: QListWidgetItem) -> None:
        device = item.data(USER_ROLE)
        if device is None:
            return
        current_auto_connect_address = self._auto_connect_address()
        enabled = item.checkState() == Qt.CheckState.Checked
        if enabled and current_auto_connect_address == device.address:
            return
        if not enabled and current_auto_connect_address != device.address:
            return
        if not set_paired_auto_connect(device.address, enabled):
            return
        if enabled:
            self.on_log(f"Auto-connect device set: {device.name}")
        else:
            self.on_log(f"Auto-connect device cleared: {device.name}")
        self._refresh_paired_list()

    def _start_saved_auto_connect(self: MainWindow) -> None:
        if self.debug_enabled and self.auto_checkbox.isChecked():
            return
        auto_connect_address = self._auto_connect_address()
        if not auto_connect_address:
            return
        for device in self.model.state.paired_devices:
            if device.address != auto_connect_address:
                continue
            self._startup_auto_connect_device = device
            self.model.set_selection(self.SelectionSource.PAIRED, device)
            self._select_paired_device(device)
            self._apply_ui()
            self.on_log(f"Auto-connecting to {device.name}...")
            self.on_connect()
            return

    def _select_paired_device(self: MainWindow, device: DeviceInfo) -> None:
        for idx in range(self.paired_list.count()):
            item = self.paired_list.item(idx)
            info = item.data(USER_ROLE)
            if info and getattr(info, "address", None) == device.address:
                if self.paired_list.currentItem() is item:
                    return
                self.paired_list.setCurrentItem(item)
                self.paired_list.scrollToItem(item)
                return

    def _delete_selected_paired(self: MainWindow) -> None:
        item = self.paired_list.currentItem()
        if item is None:
            self.on_log("Select a device first")
            return
        device = item.data(USER_ROLE)
        if device and self._remove_paired(device.address):
            self._refresh_paired_list()
            self.on_log(f"Removed: {device.name}")
        else:
            self.on_log("Failed to delete entry.")

    def _rename_selected_paired(self: MainWindow) -> None:
        item = self.paired_list.currentItem()
        if item is None:
            self.on_log("Select a saved device first")
            return
        device = item.data(USER_ROLE)
        if device is None:
            self.on_log("Failed to resolve selected device")
            return
        new_name, accepted = QInputDialog.getText(
            self,
            "Rename saved device",
            "Device name:",
            text=device.name,
        )
        if not accepted:
            return
        normalized = new_name.strip()
        if not normalized:
            self.on_log("Name cannot be empty")
            return
        if normalized == device.name:
            return
        if not rename_paired_record(device.address, normalized):
            self.on_log("Failed to rename entry.")
            return
        self._refresh_paired_list()
        self._select_paired_device(DeviceInfo(name=normalized, address=device.address))
        self.on_log(f"Renamed: {device.name} -> {normalized}")
