from __future__ import annotations

from typing import TYPE_CHECKING

from PySide6.QtGui import QCloseEvent
from PySide6.QtWidgets import QCheckBox, QFrame, QLabel

from ..core import DeviceParams
from ..ui.parameter_controls import ParameterField


if TYPE_CHECKING:
    from .app import MainWindow


class MainWindowSetupMixin:
    def _init_view_state(self: MainWindow) -> None:
        self.temp_fields: list[QLabel] = []
        self.temp_indicators: list[QFrame] = []
        self.temp_values: list[float | None] = [None] * 4
        self._temp_is_nc: list[bool | None] = [None] * 4
        self.fan_fields: list[QLabel] = []
        self.fan_percent_field: QLabel | None = None
        self._fan_is_nc: list[bool | None] = [None] * 4
        self.fan_status_indicators: list[QFrame] = []
        self.fan_monitor_checkboxes: list[QCheckBox] = []
        self.voltage_field: QLabel | None = None
        self.current_field: QLabel | None = None
        self.device_status_field: QLabel | None = None
        self.device_status_indicator: QFrame | None = None
        self.param_fields: list[ParameterField] = []
        self._params_update_lock = False
        self._device_params_snapshot: DeviceParams | None = None
        self._selected_profile_index: int | None = None
        self._device_profile_slots: set[int] = set()
        self._device_profile_metadata: dict[int, dict[str, str]] = {}
        self._apply_status_kind = "idle"
        self._apply_status_text = ""

    def _configure_action_properties(self: MainWindow) -> None:
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
            widget.setProperty("actionId", action.name)

    def _connect_signals(self: MainWindow) -> None:
        self.scan_button.clicked.connect(self.on_scan)
        self.pair_button.clicked.connect(self.on_pair)
        self.connect_button.clicked.connect(self.on_connect)
        self.disconnect_button.clicked.connect(self.on_disconnect)
        self.delete_button.clicked.connect(self.on_delete_paired_clicked)
        self.rename_button.clicked.connect(self.on_rename_paired_clicked)
        self.auto_checkbox.toggled.connect(self.on_auto_toggled)
        self.apply_button.clicked.connect(self._apply_settings_in_place)
        self.discard_button.clicked.connect(self.on_discard)
        self.calibrate_button.clicked.connect(self.on_calibrate)
        self.setup_fans_button.clicked.connect(self.on_setup_fans)
        self.clear_op_log_button.clicked.connect(self._clear_op_log)
        self.clear_debug_log_button.clicked.connect(self._clear_debug_log)
        self.clear_data_log_button.clicked.connect(self._clear_data_log)

        self.worker.log.connect(self.on_log)
        self.worker.scan_results.connect(self.on_scan_results)
        self.worker.metrics_received.connect(self.on_metrics_received)
        self.worker.params_received.connect(self.on_params_received)
        self.worker.params_command_done.connect(self.on_params_command_done)
        self.worker.profiles_received.connect(self.on_profiles_received)
        self.worker.profile_metadata_received.connect(self.on_profile_metadata_received)
        self.worker.profile_action_done.connect(self.on_profile_action_done)
        self.worker.profile_metadata_done.connect(self.on_profile_metadata_done)
        self.worker.fan_status_received.connect(self.on_fan_status)
        self.worker.device_status_received.connect(self.on_device_status)
        self.worker.operation_status_received.connect(self.on_operation_status)
        self.worker.operation_start_failed.connect(self.on_operation_start_failed)
        self.worker.output_limit_received.connect(self.on_output_limit)
        self.worker.fan_mode_status_received.connect(self.on_fan_mode_status)
        self.worker.output_limit_command_done.connect(self.on_output_limit_command_done)
        self.worker.pairing_result.connect(self.on_pairing_result)
        self.worker.connection_event.connect(self.on_connection_event)

        self.found_list.itemSelectionChanged.connect(self._on_found_selected)
        self.paired_list.itemSelectionChanged.connect(self._on_paired_selected)
        self.paired_list.itemChanged.connect(self._on_paired_item_changed)

    def closeEvent(self: MainWindow, event: QCloseEvent) -> None:
        stop_event = self.model.state.auto_run_id
        if stop_event is not None:
            self.worker.stop_auto(stop_event)
        connection = self._action_futures.get(self.Action.CONNECT)
        if connection is not None:
            connection.cancel()
        self._save_metrics_chart_history()
        disconnect = self.worker.disconnect_device(fast=True)
        fut = self.worker.submit(disconnect)
        if fut is None:
            disconnect.close()
        else:
            try:
                fut.result(timeout=4.5)
            except Exception:
                pass
        self.worker.stop()
        self.worker.wait(3000)
        super().closeEvent(event)
