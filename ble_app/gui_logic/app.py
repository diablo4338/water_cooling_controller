from __future__ import annotations

import asyncio
import sys
from typing import Optional

from PySide6.QtCore import QThread, Signal
from PySide6.QtWidgets import QApplication, QMainWindow

from ..config import DEFAULT_CONFIG
from ..core import (
    BleAppCore,
    DeviceParams,
    DeviceInfo,
    DeviceStatus,
    FanStatus,
    MetricsSnapshot,
    OperationStatus,
    ParamsStatus,
    PARAM_STATUS_BUSY,
)
from ..presentation import Action, AppModel, ConnState, SelectionSource
from .constants import APP_TITLE
from .worker_flows import BleWorkerFlowMixin
from .worker_runtime import BleWorkerRuntimeMixin
from .window_actions import MainWindowActionMixin
from .window_lists import MainWindowListMixin
from .window_params import MainWindowParamsMixin
from .window_setup import MainWindowSetupMixin
from .window_updates import MainWindowUpdateMixin


class BleWorker(BleWorkerRuntimeMixin, BleWorkerFlowMixin, QThread):
    log = Signal(str)
    scan_results = Signal(list)
    metrics_received = Signal(object)
    params_received = Signal(object)
    params_status = Signal(object)
    apply_done = Signal()
    fan_status_received = Signal(object)
    device_status_received = Signal(object)
    operation_status_received = Signal(object)
    pairing_result = Signal(bool, str, object, object)
    connection_state = Signal(bool, object)

    def __init__(self) -> None:
        super().__init__()
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.config = DEFAULT_CONFIG
        self.core = BleAppCore(log=self.log.emit, config=self.config)
        self.device: Optional[DeviceInfo] = None
        self._disconnect_evt: Optional[asyncio.Event] = None
        self._auto_stop_evt: Optional[asyncio.Event] = None
        self._auto_running = False
        self._manual_disconnect = False
        self._monitor_stop_evt: Optional[asyncio.Event] = None
        self._monitor_task: Optional[asyncio.Task] = None
        self._metrics_snapshot = MetricsSnapshot.empty()


class MainWindow(
    MainWindowSetupMixin,
    MainWindowParamsMixin,
    MainWindowListMixin,
    MainWindowActionMixin,
    MainWindowUpdateMixin,
    QMainWindow,
):
    Action = Action
    ConnState = ConnState
    SelectionSource = SelectionSource
    MetricsSnapshot = MetricsSnapshot
    PARAM_STATUS_BUSY = PARAM_STATUS_BUSY

    def __init__(self, debug: bool = False) -> None:
        super().__init__()
        self.debug_enabled = debug
        self.worker = BleWorker()
        self.worker.start()
        self.model = AppModel()
        self.model.set_status("Ready")
        self._lock_selection_active = False
        self.action_timeouts = {
            Action.SCAN: self.worker.config.gui_action_scan_timeout_s,
            Action.PAIR: self.worker.config.gui_action_pair_timeout_s,
            Action.CONNECT: self.worker.config.gui_action_connect_timeout_s,
            Action.DISCONNECT: self.worker.config.gui_action_disconnect_timeout_s,
        }
        self.default_action_timeout = self.worker.config.gui_action_default_timeout_s
        self._action_timers = {}
        self._action_futures = {}
        self.metrics_snapshot = MetricsSnapshot.empty()
        self._create_widgets()
        self._init_view_state()
        self._configure_action_properties()
        self._build_window()
        self._connect_signals()
        self._refresh_paired_list()
        self._reset_params_fields()
        self._apply_ui()


def main() -> int:
    debug = "--debug" in sys.argv
    argv = [arg for arg in sys.argv if arg != "--debug"]
    app = QApplication(argv)
    window = MainWindow(debug=debug)
    window.show()
    return app.exec()
