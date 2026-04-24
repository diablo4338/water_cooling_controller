from __future__ import annotations

from PySide6.QtGui import QBrush
from PySide6.QtWidgets import QListWidget, QListWidgetItem

from ..core import DeviceInfo, add_or_update_paired, load_paired_records, save_paired_records
from .constants import PAIRED_HIGHLIGHT_BRUSH, USER_ROLE


class MainWindowListMixin:
    def _apply_ui(self) -> None:
        ui = self.model.ui
        status = ui.status_text
        if self.model.state.connected_device:
            status = f"{status} | {self.model.state.connected_device.name}"
        self.status_label.setText(status)

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

        params_enabled = self.model.state.conn == self.ConnState.CONNECTED and not self.model.state.busy
        if self._operation_active:
            self.apply_button.setEnabled(False)
            self.discard_button.setEnabled(False)
            self.calibrate_button.setEnabled(False)
            self.setup_fans_button.setEnabled(False)
            params_enabled = False
        for item in self.param_fields:
            item["widget"].setEnabled(params_enabled)
        for idx, checkbox in enumerate(self.fan_monitor_checkboxes):
            checkbox.setEnabled(params_enabled and idx > 0)

        self.auto_checkbox.blockSignals(True)
        self.auto_checkbox.setChecked(ui.auto_enabled)
        self.auto_checkbox.blockSignals(False)
        self._apply_paired_highlight()

    def _apply_paired_highlight(self) -> None:
        highlight = self.model.ui.paired_highlight
        for idx in range(self.paired_list.count()):
            item = self.paired_list.item(idx)
            info = item.data(USER_ROLE)
            if highlight and info and getattr(info, "address", None) == highlight.address:
                item.setBackground(PAIRED_HIGHLIGHT_BRUSH)
            else:
                item.setBackground(QBrush())

    def _clear_paired_selection(self) -> None:
        self.paired_list.blockSignals(True)
        self.paired_list.clearSelection()
        self.paired_list.blockSignals(False)
        if self.model.state.selected_source == self.SelectionSource.PAIRED:
            self.model.set_selection(None, None)

    def _lock_selection_to_connected(self) -> None:
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

    def _on_found_selected(self) -> None:
        self._sync_selection(self.SelectionSource.FOUND, self.found_list, self.paired_list)

    def _on_paired_selected(self) -> None:
        self._sync_selection(self.SelectionSource.PAIRED, self.paired_list, self.found_list)

    def _sync_selection(self, source, current_list: QListWidget, other_list: QListWidget) -> None:
        if self.model.state.conn == self.ConnState.CONNECTED:
            self._lock_selection_to_connected()
            self._apply_ui()
            return
        item = current_list.currentItem()
        if item is not None:
            other_list.blockSignals(True)
            other_list.clearSelection()
            other_list.blockSignals(False)
            self.model.set_selection(source, item.data(USER_ROLE))
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

    def _refresh_paired_list(self) -> None:
        self.paired_list.clear()
        devices = self._load_paired()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(USER_ROLE, dev)
            self.paired_list.addItem(item)
        self.model.set_paired_devices(devices)
        if self.model.state.conn == self.ConnState.CONNECTED:
            self._lock_selection_to_connected()
            self._apply_ui()
            return
        prev = self.model.state.selected_device if self.model.state.selected_source == self.SelectionSource.PAIRED else None
        if prev and any(dev.address == prev.address for dev in devices):
            self._select_paired_device(prev)
        elif self.model.state.selected_source == self.SelectionSource.PAIRED:
            self.model.set_selection(None, None)
            self.paired_list.clearSelection()
        self._apply_ui()

    def _select_paired_device(self, device: DeviceInfo) -> None:
        for idx in range(self.paired_list.count()):
            item = self.paired_list.item(idx)
            info = item.data(USER_ROLE)
            if info and getattr(info, "address", None) == device.address:
                if self.paired_list.currentItem() is item:
                    return
                self.paired_list.setCurrentItem(item)
                self.paired_list.scrollToItem(item)
                return

    def _delete_selected_paired(self) -> None:
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
