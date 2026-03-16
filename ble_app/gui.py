import asyncio
import math
import sys
from concurrent.futures import Future
from typing import Callable, Optional

from PySide6.QtCore import QThread, Qt, Signal, Slot, QTimer
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QPushButton,
    QSpinBox,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from .config import DEFAULT_CONFIG
from .core import (
    BleAppCore,
    DeviceParams,
    DeviceInfo,
    FanStatus,
    OperationStatus,
    ParamsStatus,
    FAN_STATE_IDLE,
    FAN_STATE_STARTING,
    FAN_STATE_RUNNING,
    FAN_STATE_STALL,
    FAN_STATE_IN_SERVICE,
    OP_STATE_IDLE,
    OP_STATE_IN_SERVICE,
    OP_STATE_DONE,
    OP_STATE_ERROR,
    OP_STATE_NAMES,
    OP_TYPE_NONE,
    OP_TYPE_FAN_CALIBRATION,
    OP_TYPE_FAN_CONTROL_DETECT,
    OP_TYPE_NAMES,
    PARAM_STATUS_BUSY,
    UUID_CONFIG_STATUS,
    FAN_CONTROL_DC,
    FAN_CONTROL_PWM,
    FAN_CONTROL_TYPE_NAMES,
    add_or_update_paired,
    load_paired_records,
    load_device_params,
    save_device_params,
    save_paired_records,
    update_paired_last_connected,
)
from .presentation import Action, AppModel, ConnState, SelectionSource

APP_TITLE = "BLE Pairing GUI"
USER_ROLE = Qt.ItemDataRole.UserRole
PAIRED_HIGHLIGHT_BRUSH = QBrush(QColor(220, 245, 220))
FAN_CONTROL_CHOICES = [
    (FAN_CONTROL_DC, FAN_CONTROL_TYPE_NAMES[FAN_CONTROL_DC]),
    (FAN_CONTROL_PWM, FAN_CONTROL_TYPE_NAMES[FAN_CONTROL_PWM]),
]
PARAM_FIELDS = [
    {"key": "target_temp_c", "label": "Целевая температура, °C", "kind": "float"},
    {"key": "fan_min_rpm", "label": "Мин. обороты вентилятора, об/мин", "kind": "float"},
    {"key": "alarm_delta_c", "label": "Аварийная дельта, °C", "kind": "float"},
    {
        "key": "fan_min_speed",
        "label": "Мин. скорость вентилятора, %",
        "kind": "int",
        "min": 0,
        "max": 120,
        "unset": -1,
    },
    {
        "key": "fan_control_type",
        "label": "Тип управления",
        "kind": "enum",
        "choices": FAN_CONTROL_CHOICES,
    },
]
PARAM_LABELS_BY_ID = {idx: spec["label"] for idx, spec in enumerate(PARAM_FIELDS)}


class BleWorker(QThread):
    log = Signal(str)
    scan_results = Signal(list)
    temp_received = Signal(int, float)
    fan_received = Signal(float)
    params_received = Signal(object)
    params_status = Signal(object)
    apply_done = Signal()
    fan_status_received = Signal(object)
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

    def run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    @staticmethod
    def _call_soon_noargs(loop: asyncio.AbstractEventLoop, fn: Callable[[], None]) -> None:
        def _cb(_: object) -> None:
            fn()

        # noinspection PyArgumentList
        loop.call_soon_threadsafe(_cb, None)

    def stop(self) -> None:
        if self.loop:
            self._call_soon_noargs(self.loop, self.loop.stop)

    def stop_auto(self) -> None:
        if self.loop and self._auto_stop_evt is not None:
            self._call_soon_noargs(self.loop, self._auto_stop_evt.set)

    def submit(self, coro) -> Optional[Future]:
        if not self.loop:
            return None
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    async def _with_timeout(self, coro, label: str, timeout: Optional[float] = None):
        try:
            use_timeout = self.config.gui_action_default_timeout_s if timeout is None else timeout
            return await asyncio.wait_for(coro, timeout=use_timeout)
        except asyncio.TimeoutError as exc:
            raise RuntimeError(f"Timeout: {label}") from exc

    def _on_disconnected(self, _) -> None:
        if self._disconnect_evt is not None and self._disconnect_evt.is_set():
            return
        if self._disconnect_evt is not None:
            self._disconnect_evt.set()
        if self._monitor_stop_evt is not None:
            self._monitor_stop_evt.set()
        if self._manual_disconnect:
            return
        self.log.emit("Соединение разорвано устройством.")
        self.connection_state.emit(False, None)

    def _start_monitor(self) -> None:
        if self._monitor_task is not None:
            return
        self._monitor_stop_evt = asyncio.Event()
        self._monitor_task = asyncio.create_task(self._monitor_connection())

    async def _monitor_connection(self) -> None:
        try:
            ping_interval = 2.0
            ping_every = 3
            counter = 0
            while self._monitor_stop_evt is not None and not self._monitor_stop_evt.is_set():
                client = self.core.client
                if client is None:
                    break
                if not getattr(client, "is_connected", False):
                    self._on_disconnected(None)
                    break
                counter += 1
                if counter >= ping_every:
                    counter = 0
                    try:
                        await asyncio.wait_for(
                            client.read_gatt_char(UUID_CONFIG_STATUS),
                            timeout=1.0,
                        )
                    except Exception:
                        self._on_disconnected(None)
                        break
                await asyncio.sleep(ping_interval)
        finally:
            self._monitor_task = None
            self._monitor_stop_evt = None

    async def scan(self) -> None:
        self.log.emit("Поиск устройств в режиме сопряжения...")
        try:
            devices = await self.core.scan_pairing(timeout=self.config.scan_timeout_s)
        except Exception as exc:
            self.log.emit(f"Ошибка сканирования: {exc}")
            self.scan_results.emit([])
            return

        self.scan_results.emit(devices)
        self.log.emit(f"Найдено устройств готовых к сопряжению: {len(devices)}")

    async def auto_connect_saved(self) -> None:
        if self._auto_running:
            self.log.emit("Автоподключение уже запущено.")
            return
        self._auto_running = True
        self._auto_stop_evt = asyncio.Event()
        try:
            while not self._auto_stop_evt.is_set():
                saved = self._load_saved_devices()
                if not saved:
                    self.log.emit("Нет сохраненных устройств.")
                    try:
                        await asyncio.wait_for(self._auto_stop_evt.wait(), timeout=2.0)
                    except asyncio.TimeoutError:
                        pass
                    continue
                for device in saved:
                    if self._auto_stop_evt.is_set():
                        break
                    await self.connect_device(device)
                    if self.core.client:
                        await self._wait_disconnect_or_stop()
                    if self._auto_stop_evt.is_set():
                        break
        finally:
            self._auto_running = False
            self._auto_stop_evt = None

    async def _wait_disconnect_or_stop(self) -> None:
        if self._disconnect_evt is None or self._auto_stop_evt is None:
            return
        while True:
            if self._disconnect_evt.is_set() or self._auto_stop_evt.is_set():
                break
            client = self.core.client
            if client is None or not getattr(client, "is_connected", False):
                self._on_disconnected(None)
                break
            await asyncio.sleep(0.5)
        if self._auto_stop_evt.is_set():
            await self.disconnect()

    @staticmethod
    def _load_saved_devices() -> list[DeviceInfo]:
        devices: list[DeviceInfo] = []
        records = load_paired_records()
        records.sort(key=lambda r: r.get("last_connected", 0), reverse=True)
        for item in records:
            name = item.get("name", "Unknown")
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=name, address=address))
        return devices

    async def connect_device(self, device: DeviceInfo) -> None:
        await self.disconnect()
        self.log.emit(f"Подключение к {device.name} ({device.address})...")
        last_exc: Optional[Exception] = None
        self._disconnect_evt = asyncio.Event()
        for attempt in range(1, 2):
            self.log.emit(f"Connect attempt {attempt} to {device.address}")
            try:
                await self.core.connect_raw(device, connect_timeout=self.config.connect_timeout_s)
                if self.core.client and hasattr(self.core.client, "set_disconnected_callback"):
                    self.core.client.set_disconnected_callback(self._on_disconnected)
                self.log.emit("Connected, starting AUTH...")
                await self._do_auth()
                self.log.emit("AUTH ok, reading initial PARAMS...")
                try:
                    params = await self.core.read_params(timeout=self.config.metrics_timeout_s)
                    self.params_received.emit(params)
                    self.log.emit("Initial PARAMS read ok.")
                except Exception as exc:
                    self.log.emit(f"Initial PARAMS read failed: {exc}")
                self.log.emit("AUTH ok, reading initial FAN status...")
                try:
                    status = await self.core.read_fan_status(timeout=self.config.metrics_timeout_s)
                    self.fan_status_received.emit(status)
                    self.log.emit("Initial FAN status read ok.")
                except Exception as exc:
                    self.log.emit(f"Initial FAN status read failed: {exc}")
                self.log.emit("AUTH ok, reading initial OP status...")
                try:
                    status = await self.core.read_operation_status(timeout=self.config.metrics_timeout_s)
                    self.operation_status_received.emit(status)
                    self.log.emit("Initial OP status read ok.")
                except Exception as exc:
                    self.log.emit(f"Initial OP status read failed: {exc}")
                self.log.emit("AUTH ok, reading initial METRICS...")
                try:
                    values = await self.core.read_metrics(timeout=self.config.metrics_timeout_s)
                    for idx, value in enumerate(values):
                        self.temp_received.emit(idx, value)
                    self.log.emit("Initial METRICS read ok.")
                except Exception as exc:
                    self.log.emit(f"Initial METRICS read failed: {exc}")
                try:
                    rpm = await self.core.read_fan_speed(timeout=self.config.metrics_timeout_s)
                    self.fan_received.emit(rpm)
                    self.log.emit("Initial FAN speed read ok.")
                except Exception as exc:
                    self.log.emit(f"Initial FAN speed read failed: {exc}")
                self.log.emit("Starting notify...")
                try:
                    await self._with_timeout(
                        self.core.start_metrics_notify(self._on_temp, self._on_fan),
                        f"start_notify#{attempt}",
                    )
                    self.log.emit("Notify METRICS started.")
                except Exception as exc:
                    self.log.emit(f"Notify METRICS недоступен: {exc}")
                try:
                    await self._with_timeout(
                        self.core.start_params_notify(self._on_params_status),
                        f"start_params_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit("Notify PARAMS status started.")
                except Exception as exc:
                    self.log.emit(f"Notify PARAMS status недоступен: {exc}")
                try:
                    await self._with_timeout(
                        self.core.start_fan_status_notify(self._on_fan_status),
                        f"start_fan_status_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit("Notify FAN status started.")
                except Exception as exc:
                    self.log.emit(f"Notify FAN status недоступен: {exc}")
                try:
                    await self._with_timeout(
                        self.core.start_operation_status_notify(self._on_operation_status),
                        f"start_operation_status_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit("Notify OP status started.")
                except Exception as exc:
                    self.log.emit(f"Notify OP status недоступен: {exc}")
                self._start_monitor()
                self.device = device
                self.connection_state.emit(True, device)
                return
            except Exception as exc:
                last_exc = exc
                self.log.emit(f"Connect attempt {attempt} failed: {exc}")
                # noinspection PyBroadException
                try:
                    await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
                except Exception:
                    pass
                await asyncio.sleep(0.4 * attempt)
        self.log.emit(f"Ошибка подключения: {last_exc}")
        if self._disconnect_evt is None or not self._disconnect_evt.is_set():
            self.connection_state.emit(False, None)

    async def disconnect(self) -> None:
        already_disconnected = self._disconnect_evt.is_set() if self._disconnect_evt is not None else False
        self._manual_disconnect = True
        try:
            if self.core.client:
                log_enabled = not already_disconnected
                if log_enabled:
                    self.log.emit("Отключение...")
                if self._monitor_stop_evt is not None:
                    self._monitor_stop_evt.set()
                # noinspection PyBroadException
                try:
                    if log_enabled:
                        self.log.emit("Stopping notify METRICS...")
                    await self._with_timeout(self.core.stop_metrics_notify(), "stop_notify", timeout=3.0)
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit("Stopping notify PARAMS...")
                    await self._with_timeout(
                        self.core.stop_params_notify(), "stop_params_notify", timeout=3.0
                    )
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit("Stopping notify FAN status...")
                    await self._with_timeout(
                        self.core.stop_fan_status_notify(), "stop_fan_status_notify", timeout=3.0
                    )
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit("Stopping notify OP status...")
                    await self._with_timeout(
                        self.core.stop_operation_status_notify(),
                        "stop_operation_status_notify",
                        timeout=3.0,
                    )
                except Exception:
                    pass
                # noinspection PyBroadException
                try:
                    if log_enabled:
                        self.log.emit("Disconnecting BLE...")
                    await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
                except Exception:
                    pass
            self.device = None
            if self._disconnect_evt is not None:
                self._disconnect_evt.set()
            if not already_disconnected:
                self.connection_state.emit(False, None)
        finally:
            self._manual_disconnect = False

    async def pair(self, device: DeviceInfo) -> None:
        result = await self.core.pair(device)
        if result.ok:
            self.pairing_result.emit(True, "Устройство спарено", device, result.k_hex)
            return
        self.pairing_result.emit(False, f"Ошибка спаривания: {result.message}", device, None)

    async def _do_auth(self) -> None:
        await self.core.auth(timeout=self.config.auth_timeout_s)

    async def write_params(self, params: DeviceParams) -> None:
        try:
            await self._with_timeout(
                self.core.write_params(params, timeout=self.config.metrics_timeout_s),
                "write_params",
                timeout=self.config.metrics_timeout_s,
            )
        except Exception as exc:
            self.log.emit(f"Ошибка отправки параметров: {exc}")

    async def apply_params(self) -> None:
        try:
            await self._with_timeout(
                self.core.apply_params(timeout=self.config.metrics_timeout_s),
                "apply_params",
                timeout=self.config.metrics_timeout_s,
            )
        except Exception as exc:
            self.log.emit(f"Ошибка Apply: {exc}")
        finally:
            self.apply_done.emit()

    async def start_fan_calibration(self) -> None:
        try:
            await self._with_timeout(
                self.core.start_fan_calibration(timeout=self.config.metrics_timeout_s),
                "fan_calibration",
                timeout=self.config.metrics_timeout_s,
            )
            self.log.emit("Запрос калибровки отправлен")
        except Exception as exc:
            self.log.emit(f"Ошибка калибровки вентилятора: {exc}")

    async def start_fan_control_detect(self) -> None:
        try:
            await self._with_timeout(
                self.core.start_fan_control_detect(timeout=self.config.metrics_timeout_s),
                "fan_control_detect",
                timeout=self.config.metrics_timeout_s,
            )
            self.log.emit("Запрос определения управления вентилятором отправлен")
        except Exception as exc:
            self.log.emit(f"Ошибка определения управления вентилятором: {exc}")

    async def read_operation_status(self) -> None:
        try:
            status = await self._with_timeout(
                self.core.read_operation_status(timeout=self.config.metrics_timeout_s),
                "read_operation_status",
                timeout=self.config.metrics_timeout_s,
            )
            self.operation_status_received.emit(status)
            self.log.emit("OP статус обновлен.")
        except Exception as exc:
            self.log.emit(f"Ошибка чтения OP статуса: {exc}")

    async def read_params_snapshot(self) -> None:
        try:
            params = await self._with_timeout(
                self.core.read_params(timeout=self.config.metrics_timeout_s),
                "read_params_snapshot",
                timeout=self.config.metrics_timeout_s,
            )
            self.params_received.emit(params)
            self.log.emit("Параметры обновлены после операции.")
        except Exception as exc:
            self.log.emit(f"Ошибка чтения параметров после операции: {exc}")

    def _on_temp(self, channel: int, value: float) -> None:
        self.temp_received.emit(channel, value)

    def _on_fan(self, value: float) -> None:
        self.fan_received.emit(value)

    def _on_params_status(self, status: ParamsStatus) -> None:
        self.params_status.emit(status)

    def _on_fan_status(self, status: FanStatus) -> None:
        self.fan_status_received.emit(status)

    def _on_operation_status(self, status: OperationStatus) -> None:
        self.operation_status_received.emit(status)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(900, 600)

        self.worker = BleWorker()
        self.worker.start()
        self.model = AppModel()
        self.model.set_status("Готово")
        self._lock_selection_active = False
        self.action_timeouts = {
            Action.SCAN: self.worker.config.gui_action_scan_timeout_s,
            Action.PAIR: self.worker.config.gui_action_pair_timeout_s,
            Action.CONNECT: self.worker.config.gui_action_connect_timeout_s,
            Action.DISCONNECT: self.worker.config.gui_action_disconnect_timeout_s,
        }
        self.default_action_timeout = self.worker.config.gui_action_default_timeout_s

        self.scan_button = QPushButton("Сканировать")
        self.pair_button = QPushButton("Спарить")
        self.connect_button = QPushButton("Подключить")
        self.disconnect_button = QPushButton("Отключить")
        self.delete_button = QPushButton("Удалить сохраненное")
        self.auto_checkbox = QCheckBox("Автоподключение (сохраненные)")
        self.apply_button = QPushButton("Apply")
        self.discard_button = QPushButton("Discard")
        self.calibrate_button = QPushButton("Калибровка вентилятора")
        self.detect_button = QPushButton("Определить управление вентилятором")
        self._action_timers: dict[Action, QTimer] = {}
        self._action_futures: dict[Action, Future] = {}

        self.scan_button.setProperty("actionId", Action.SCAN.name)
        self.pair_button.setProperty("actionId", Action.PAIR.name)
        self.connect_button.setProperty("actionId", Action.CONNECT.name)
        self.disconnect_button.setProperty("actionId", Action.DISCONNECT.name)
        self.apply_button.setProperty("actionId", Action.APPLY.name)
        self.discard_button.setProperty("actionId", Action.DISCARD.name)
        self.calibrate_button.setProperty("actionId", Action.CALIBRATE.name)
        self.detect_button.setProperty("actionId", Action.DETECT_FAN_CONTROL.name)
        self.delete_button.setProperty("actionId", Action.DELETE_PAIRED.name)
        self.auto_checkbox.setProperty("actionId", Action.AUTO_CONNECT.name)

        self.found_list = QListWidget()
        self.paired_list = QListWidget()
        self.data_view = QTextEdit()
        self.data_view.setReadOnly(True)
        self.status_label = QLabel(self.model.state.status)
        self.temp_fields: list[QLineEdit] = []
        self._temp_is_nc: list[Optional[bool]] = [None] * 4
        self.fan_field: Optional[QLineEdit] = None
        self._fan_is_nc: Optional[bool] = None
        self.fan_status_field: Optional[QLineEdit] = None
        self._operation_active = False
        self.param_fields: list[dict] = []
        self._params_update_lock = False

        buttons = QHBoxLayout()
        buttons.addWidget(self.scan_button)
        buttons.addWidget(self.pair_button)
        buttons.addWidget(self.connect_button)
        buttons.addWidget(self.disconnect_button)
        buttons.addWidget(self.delete_button)
        buttons.addWidget(self.auto_checkbox)

        lists_layout = QHBoxLayout()
        lists_layout.addWidget(self._wrap_list("Устройства для сопряжения", self.found_list))
        lists_layout.addWidget(self._wrap_list("Спаренные устройства", self.paired_list))

        layout = QVBoxLayout()
        layout.addLayout(buttons)
        layout.addLayout(lists_layout)
        layout.addWidget(QLabel("Параметры устройства"))
        layout.addWidget(self._build_params_layout())
        layout.addWidget(QLabel("Температуры"))
        layout.addLayout(self._build_temp_layout())
        layout.addWidget(QLabel("Скорость вентилятора"))
        layout.addLayout(self._build_fan_layout())
        layout.addWidget(QLabel("Данные в реальном времени"))
        layout.addWidget(self.data_view)
        layout.addWidget(self.status_label)

        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)

        self.scan_button.clicked.connect(self.on_scan)
        self.pair_button.clicked.connect(self.on_pair)
        self.connect_button.clicked.connect(self.on_connect)
        self.disconnect_button.clicked.connect(self.on_disconnect)
        self.delete_button.clicked.connect(self.on_delete_paired_clicked)
        self.auto_checkbox.toggled.connect(self.on_auto_toggled)
        self.apply_button.clicked.connect(self.on_apply)
        self.discard_button.clicked.connect(self.on_discard)
        self.calibrate_button.clicked.connect(self.on_calibrate)
        self.detect_button.clicked.connect(self.on_detect_fan_control)

        self.worker.log.connect(self.on_log)
        self.worker.scan_results.connect(self.on_scan_results)
        self.worker.temp_received.connect(self.on_temp_received)
        self.worker.fan_received.connect(self.on_fan_received)
        self.worker.params_received.connect(self.on_params_received)
        self.worker.params_status.connect(self.on_params_status)
        self.worker.apply_done.connect(self.on_apply_done)
        self.worker.fan_status_received.connect(self.on_fan_status)
        self.worker.operation_status_received.connect(self.on_operation_status)
        self.worker.pairing_result.connect(self.on_pairing_result)
        self.worker.connection_state.connect(self.on_connection_state)

        self.found_list.itemSelectionChanged.connect(self._on_found_selected)
        self.paired_list.itemSelectionChanged.connect(self._on_paired_selected)

        self._refresh_paired_list()
        self._reset_params_fields()
        self._apply_ui()

    def closeEvent(self, event) -> None:
        self.worker.stop_auto()
        fut = self.worker.submit(self.worker.disconnect())
        if fut is not None:
            # noinspection PyBroadException
            try:
                fut.result(timeout=3)
            except Exception:
                pass
        self.worker.stop()
        super().closeEvent(event)

    @staticmethod
    def _wrap_list(title: str, widget: QListWidget) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        layout.addWidget(QLabel(title))
        layout.addWidget(widget)
        wrapper.setLayout(layout)
        return wrapper

    def _build_params_layout(self) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        grid = QGridLayout()
        self.param_fields.clear()

        for idx, spec in enumerate(PARAM_FIELDS):
            grid.addWidget(QLabel(spec["label"]), idx, 0)
            kind = spec["kind"]
            if kind == "float":
                field = QDoubleSpinBox()
                field.setRange(-1_000_000.0, 1_000_000.0)
                field.setSpecialValueText("—")
                field.setDecimals(2)
                field.setSingleStep(0.5)
                field.setAlignment(Qt.AlignmentFlag.AlignRight)
                field.setKeyboardTracking(False)
                field.valueChanged.connect(self._on_params_changed)
                field.setValue(field.minimum())
            elif kind == "int":
                field = QSpinBox()
                min_val = spec.get("min", -1_000_000)
                max_val = spec.get("max", 1_000_000)
                unset_val = spec.get("unset", min_val)
                field.setRange(unset_val, max_val)
                field.setSingleStep(1)
                field.setSpecialValueText("—")
                field.setAlignment(Qt.AlignmentFlag.AlignRight)
                field.setKeyboardTracking(False)
                field.valueChanged.connect(self._on_params_changed)
                field.setValue(unset_val)
            elif kind == "enum":
                field = QComboBox()
                field.addItem("—", None)
                for value, label in spec.get("choices", []):
                    field.addItem(label, value)
                field.currentIndexChanged.connect(self._on_params_changed)
                field.setCurrentIndex(0)
            else:
                continue
            field.setEnabled(False)
            grid.addWidget(field, idx, 1)
            self.param_fields.append({"spec": spec, "widget": field})

        button_row = QHBoxLayout()
        button_row.addStretch(1)
        button_row.addWidget(self.apply_button)
        button_row.addWidget(self.discard_button)

        layout.addLayout(grid)
        layout.addLayout(button_row)
        wrapper.setLayout(layout)
        return wrapper

    def _build_temp_layout(self) -> QGridLayout:
        grid = QGridLayout()
        self.temp_fields.clear()
        self._temp_is_nc = [None] * 4
        for idx in range(4):
            label = QLabel(f"Темп. {idx + 1}")
            field = QLineEdit("—")
            field.setReadOnly(True)
            field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            field.setCursor(Qt.CursorShape.ArrowCursor)
            field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            field.setAlignment(Qt.AlignmentFlag.AlignRight)
            row = idx // 2
            col = (idx % 2) * 2
            grid.addWidget(label, row, col)
            grid.addWidget(field, row, col + 1)
            self.temp_fields.append(field)
        return grid

    def _build_fan_layout(self) -> QGridLayout:
        grid = QGridLayout()
        rpm_label = QLabel("Вентилятор (об/мин)")
        rpm_field = QLineEdit("—")
        rpm_field.setReadOnly(True)
        rpm_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        rpm_field.setCursor(Qt.CursorShape.ArrowCursor)
        rpm_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        rpm_field.setAlignment(Qt.AlignmentFlag.AlignRight)
        grid.addWidget(rpm_label, 0, 0)
        grid.addWidget(rpm_field, 0, 1)
        self.fan_field = rpm_field

        status_label = QLabel("Статус вентилятора")
        status_field = QLineEdit("—")
        status_field.setReadOnly(True)
        status_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        status_field.setCursor(Qt.CursorShape.ArrowCursor)
        status_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        status_field.setAlignment(Qt.AlignmentFlag.AlignRight)
        grid.addWidget(status_label, 1, 0)
        grid.addWidget(status_field, 1, 1)
        self.fan_status_field = status_field
        grid.addWidget(self.calibrate_button, 2, 0, 1, 2)
        grid.addWidget(self.detect_button, 3, 0, 1, 2)
        self._fan_is_nc = None
        return grid

    def _reset_params_fields(self) -> None:
        if not self.param_fields:
            return
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
        finally:
            self._params_update_lock = False

    def _set_params_fields(self, params: DeviceParams, save: bool = True) -> None:
        if len(self.param_fields) != len(PARAM_FIELDS):
            return
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
        finally:
            self._params_update_lock = False
        if save:
            device = self.model.state.connected_device
            if device:
                save_device_params(device.address, params)

    def _current_params(self) -> DeviceParams:
        if len(self.param_fields) != len(PARAM_FIELDS):
            device = self.model.state.connected_device
            if device:
                return load_device_params(device.address)
            return DeviceParams(
                target_temp_c=0.0,
                fan_min_rpm=0.0,
                alarm_delta_c=0.0,
                fan_min_speed=0,
                fan_control_type=FAN_CONTROL_DC,
            )
        values: dict[str, object] = {}
        for item in self.param_fields:
            spec = item["spec"]
            widget = item["widget"]
            kind = spec["kind"]
            if kind == "enum":
                data = widget.currentData()
                value = int(data) if data is not None else FAN_CONTROL_DC
            else:
                value = widget.value()
                if kind == "int":
                    value = int(value)
                    if value < 0:
                        value = 0
            values[spec["key"]] = value
        return DeviceParams(
            target_temp_c=float(values["target_temp_c"]),
            fan_min_rpm=float(values["fan_min_rpm"]),
            alarm_delta_c=float(values["alarm_delta_c"]),
            fan_min_speed=int(values["fan_min_speed"]),
            fan_control_type=int(values["fan_control_type"]),
        )

    def _on_params_changed(self, _) -> None:
        if self._params_update_lock:
            return
        params = self._current_params()
        device = self.model.state.connected_device
        if device is None or self.model.state.conn != ConnState.CONNECTED:
            return
        save_device_params(device.address, params)
        fut = self.worker.submit(self.worker.write_params(params))
        if fut is None:
            self.on_log("Не удалось отправить параметры (worker не готов).")

    def _apply_ui(self) -> None:
        ui = self.model.ui
        status = ui.status_text
        if self.model.state.connected_device:
            status = f"{status} | {self.model.state.connected_device.name}"
        self.status_label.setText(status)

        self.scan_button.setEnabled(Action.SCAN in ui.enabled_actions)
        self.pair_button.setEnabled(Action.PAIR in ui.enabled_actions)
        self.connect_button.setEnabled(Action.CONNECT in ui.enabled_actions)
        self.disconnect_button.setEnabled(Action.DISCONNECT in ui.enabled_actions)
        self.apply_button.setEnabled(Action.APPLY in ui.enabled_actions)
        self.discard_button.setEnabled(Action.DISCARD in ui.enabled_actions)
        self.calibrate_button.setEnabled(Action.CALIBRATE in ui.enabled_actions)
        self.detect_button.setEnabled(Action.DETECT_FAN_CONTROL in ui.enabled_actions)
        self.delete_button.setEnabled(Action.DELETE_PAIRED in ui.enabled_actions)
        self.auto_checkbox.setEnabled(Action.AUTO_CONNECT in ui.enabled_actions)
        params_enabled = self.model.state.conn == ConnState.CONNECTED and not self.model.state.busy
        if self._operation_active:
            self.apply_button.setEnabled(False)
            self.discard_button.setEnabled(False)
            self.calibrate_button.setEnabled(False)
            self.detect_button.setEnabled(False)
            params_enabled = False
        for item in self.param_fields:
            item["widget"].setEnabled(params_enabled)

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
        if self.model.state.selected_source == SelectionSource.PAIRED:
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

            if target_item is not None:
                self.model.set_selection(SelectionSource.PAIRED, device)
            else:
                self.model.set_selection(None, None)
        finally:
            self._lock_selection_active = False

    def _on_found_selected(self) -> None:
        self._sync_selection(
            source=SelectionSource.FOUND,
            current_list=self.found_list,
            other_list=self.paired_list,
        )

    def _on_paired_selected(self) -> None:
        self._sync_selection(
            source=SelectionSource.PAIRED,
            current_list=self.paired_list,
            other_list=self.found_list,
        )

    def _sync_selection(
        self, source: SelectionSource, current_list: QListWidget, other_list: QListWidget
    ) -> None:
        if self.model.state.conn == ConnState.CONNECTED:
            self._lock_selection_to_connected()
            self._apply_ui()
            return
        item = current_list.currentItem()
        if item is not None:
            other_list.blockSignals(True)
            other_list.clearSelection()
            other_list.blockSignals(False)
            device = item.data(USER_ROLE)
            self.model.set_selection(source, device)
        else:
            self.model.set_selection(None, None)
        self._apply_ui()

    @staticmethod
    def _load_paired() -> list[DeviceInfo]:
        raw = load_paired_records()
        devices = []
        for item in raw:
            name = item.get("name", "Unknown")
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=name, address=address))
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

    def _start_action(
        self,
        action: Action,
        coro,
        timeout_override: Optional[float] = None,
        use_timeout: bool = True,
    ) -> None:
        timeout_s = (
            timeout_override
            if timeout_override is not None
            else self.action_timeouts.get(action, self.default_action_timeout)
        )
        if use_timeout:
            timer = QTimer(self)
            timer.setSingleShot(True)
            timer.timeout.connect(lambda: self._action_timeout(action))
            timer.start(int(timeout_s * 1000))
            self._action_timers[action] = timer

        fut = self.worker.submit(coro)
        if fut is None:
            self._finish_action(action)
            return
        self._action_futures[action] = fut

    def _action_timeout(self, action: Action) -> None:
        fut = self._action_futures.get(action)
        if fut and not fut.done():
            fut.cancel()
        if action in {Action.CALIBRATE, Action.DETECT_FAN_CONTROL}:
            status_fut = self.worker.submit(self.worker.read_operation_status())
            if status_fut is None:
                self.on_log("Не удалось запросить OP статус (worker не готов).")
            else:
                self.on_log("Запрошен OP статус по таймауту отправки.")
        self.on_log(f"Операция '{action.name}' превышает таймаут отправки.")
        self._finish_action(action)

    def _finish_action(self, action: Action) -> None:
        timer = self._action_timers.pop(action, None)
        if timer:
            timer.stop()
        self._action_futures.pop(action, None)
        self.model.finish_action(action)
        self._apply_ui()

    def _dispatch_action(self, action: Action) -> None:
        command = self.model.dispatch(action)
        self._apply_ui()
        if command is None:
            return
        if command.action == Action.SCAN:
            self._start_action(Action.SCAN, self.worker.scan())
        elif command.action == Action.PAIR and command.device:
            self._start_action(Action.PAIR, self.worker.pair(command.device))
        elif command.action == Action.CONNECT and command.device:
            self.model.start_connecting()
            self._apply_ui()
            self._start_action(
                Action.CONNECT,
                self.worker.connect_device(command.device),
                timeout_override=self.action_timeouts[Action.CONNECT],
            )
        elif command.action == Action.DISCONNECT:
            self._start_action(
                Action.DISCONNECT,
                self.worker.disconnect(),
                timeout_override=self.action_timeouts[Action.DISCONNECT],
            )
        elif command.action == Action.APPLY:
            self._start_action(Action.APPLY, self.worker.apply_params())
        elif command.action == Action.DISCARD:
            self._start_action(Action.DISCARD, self.worker.read_params_snapshot())
        elif command.action == Action.CALIBRATE:
            self._start_action(
                Action.CALIBRATE,
                self.worker.start_fan_calibration(),
                timeout_override=10.0,
            )
        elif command.action == Action.DETECT_FAN_CONTROL:
            self._start_action(
                Action.DETECT_FAN_CONTROL,
                self.worker.start_fan_control_detect(),
                timeout_override=10.0,
            )
        elif command.action == Action.DELETE_PAIRED:
            self._delete_selected_paired()

    def on_scan(self) -> None:
        self._dispatch_action(Action.SCAN)

    def on_pair(self) -> None:
        self._dispatch_action(Action.PAIR)

    def on_connect(self) -> None:
        self._dispatch_action(Action.CONNECT)

    def on_disconnect(self) -> None:
        self._dispatch_action(Action.DISCONNECT)

    def on_apply(self) -> None:
        self._dispatch_action(Action.APPLY)

    def on_discard(self) -> None:
        self._dispatch_action(Action.DISCARD)

    def on_calibrate(self) -> None:
        self._dispatch_action(Action.CALIBRATE)

    def on_detect_fan_control(self) -> None:
        self._dispatch_action(Action.DETECT_FAN_CONTROL)

    def on_delete_paired_clicked(self) -> None:
        self._dispatch_action(Action.DELETE_PAIRED)

    def _refresh_paired_list(self) -> None:
        self.paired_list.clear()
        devices = self._load_paired()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(USER_ROLE, dev)
            self.paired_list.addItem(item)
        self.model.set_paired_devices(devices)
        if self.model.state.conn == ConnState.CONNECTED:
            self._lock_selection_to_connected()
            self._apply_ui()
            return
        prev = (
            self.model.state.selected_device
            if self.model.state.selected_source == SelectionSource.PAIRED
            else None
        )
        if prev and any(dev.address == prev.address for dev in devices):
            self._select_paired_device(prev)
        elif self.model.state.selected_source == SelectionSource.PAIRED:
            self.model.set_selection(None, None)
            self.paired_list.clearSelection()
        self._apply_ui()

    def _delete_selected_paired(self) -> None:
        item = self.paired_list.currentItem()
        if item is None:
            self.on_log("Сначала выберите устройство")
            return
        device = item.data(USER_ROLE)
        if device and self._remove_paired(device.address):
            self._refresh_paired_list()
            self.on_log(f"Удалено: {device.name}")
        else:
            self.on_log("Не удалось удалить запись.")

    @Slot(list)
    def on_scan_results(self, devices: list) -> None:
        if self.model.state.active_action != Action.SCAN:
            return
        self.found_list.clear()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(USER_ROLE, dev)
            self.found_list.addItem(item)
        self.model.set_found_devices(devices)
        if self.model.state.selected_source == SelectionSource.FOUND:
            self.model.set_selection(None, None)
        self._finish_action(Action.SCAN)

    @Slot(str)
    def on_log(self, message: str) -> None:
        self.model.set_status(message)
        print(message, flush=True)
        self._apply_ui()

    @Slot(int, float)
    def on_temp_received(self, channel: int, value: float) -> None:
        if 0 <= channel < len(self.temp_fields):
            is_nc = not math.isfinite(value)
            prev_nc = self._temp_is_nc[channel]
            if prev_nc is not None and prev_nc != is_nc:
                if is_nc:
                    self.on_log(f"Канал {channel + 1}: NC (нет датчика)")
                else:
                    self.on_log(f"Канал {channel + 1}: онлайн")
            self._temp_is_nc[channel] = is_nc
            if not is_nc:
                text = f"{value:.2f}"
            else:
                text = "NC"
            self.temp_fields[channel].setText(text)
            self.data_view.append(f"Темп. {channel + 1}: {text}")

    @Slot(float)
    def on_fan_received(self, value: float) -> None:
        if self.fan_field is None:
            return
        is_nc = (not math.isfinite(value)) or value <= 0.0
        if self._fan_is_nc is not None and self._fan_is_nc != is_nc:
            if is_nc:
                self.on_log("Вентилятор: NC (остановлен)")
            else:
                self.on_log("Вентилятор: онлайн")
        self._fan_is_nc = is_nc
        if is_nc:
            text = "NC"
        else:
            text = f"{value:.0f}"
        self.fan_field.setText(text)
        self.data_view.append(f"Вентилятор: {text}")

    @Slot(object)
    def on_params_received(self, params: DeviceParams) -> None:
        self._set_params_fields(params, save=True)
        if self.model.state.active_action == Action.DISCARD:
            self._finish_action(Action.DISCARD)

    @Slot(object)
    def on_params_status(self, status: ParamsStatus) -> None:
        if self.model.state.active_action == Action.APPLY:
            QTimer.singleShot(1000, lambda: self._finish_action(Action.APPLY))
        if status.ok:
            self.on_log("Параметры применены")
            return
        if status.status == PARAM_STATUS_BUSY:
            self.on_log("Параметры не применены: устройство занято")
            return
        field_label = (
            PARAM_LABELS_BY_ID.get(status.field_id, "неизвестное поле")
            if status.field_id is not None
            else "неизвестное поле"
        )
        self.on_log(f"Ошибка параметров: {field_label}")

    @Slot(object)
    def on_fan_status(self, status: FanStatus) -> None:
        if self.fan_status_field is None:
            return
        if status.state == FAN_STATE_IDLE:
            self.fan_status_field.setText("IDLE")
        elif status.state == FAN_STATE_STARTING:
            self.fan_status_field.setText("STARTING")
        elif status.state == FAN_STATE_RUNNING:
            self.fan_status_field.setText("RUNNING")
        elif status.state == FAN_STATE_STALL:
            self.fan_status_field.setText("STALL")
        elif status.state == FAN_STATE_IN_SERVICE:
            if status.op_type != OP_TYPE_NONE:
                self.fan_status_field.setText(f"IN_SERVICE ({status.op_label})")
            else:
                self.fan_status_field.setText("IN_SERVICE")
        self._apply_ui()

    @Slot(object)
    def on_operation_status(self, status: OperationStatus) -> None:
        op_label = OP_TYPE_NAMES.get(status.op_type, f"OP{status.op_type}")
        state_label = OP_STATE_NAMES.get(status.state, "UNKNOWN")
        op_action = None
        if status.op_type == OP_TYPE_FAN_CALIBRATION:
            op_action = Action.CALIBRATE
        elif status.op_type == OP_TYPE_FAN_CONTROL_DETECT:
            op_action = Action.DETECT_FAN_CONTROL
        if status.state == OP_STATE_IN_SERVICE:
            self.on_log(f"Операция запущена: {op_label}")
            self._operation_active = True
        elif status.state == OP_STATE_DONE:
            self.on_log(f"Операция завершена: {op_label}")
            self._operation_active = False
            if op_action:
                self._finish_action(op_action)
                self.worker.submit(self.worker.read_params_snapshot())
        elif status.state == OP_STATE_ERROR:
            err_text = status.error or "неизвестная ошибка"
            if err_text.strip().lower() == "busy":
                self.on_log(f"Операция занята: {op_label}")
                self._operation_active = True
            else:
                self.on_log(f"Ошибка операции {op_label}: {err_text}")
                self._operation_active = False
                if op_action:
                    self._finish_action(op_action)
        elif status.state == OP_STATE_IDLE:
            self._operation_active = False
            if op_action:
                self._finish_action(op_action)
        else:
            self.on_log(f"Операция {op_label}: {state_label}")
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
        if self.model.state.active_action == Action.PAIR:
            self._finish_action(Action.PAIR)

    @Slot(bool, object)
    def on_connection_state(self, connected: bool, device: Optional[DeviceInfo]) -> None:
        if connected and device:
            self.model.set_connected(True, device)
            update_paired_last_connected(device.address)
            self._lock_selection_to_connected()
            if self.model.state.active_action == Action.CONNECT:
                self._finish_action(Action.CONNECT)
            self.on_log(f"Подключено к {device.name}")
        elif not connected:
            already_disconnected = self.model.state.conn == ConnState.DISCONNECTED
            self.model.set_connected(False, None)
            self._clear_paired_selection()
            self._reset_temp_fields()
            self._reset_params_fields()
            if self.model.state.active_action == Action.DISCONNECT:
                self._finish_action(Action.DISCONNECT)
            elif self.model.state.active_action == Action.CONNECT:
                self._finish_action(Action.CONNECT)
            if not already_disconnected:
                self.on_log("Отключено")

    def _reset_temp_fields(self) -> None:
        for field in self.temp_fields:
            field.setText("—")
        self._temp_is_nc = [None] * len(self.temp_fields)
        if self.fan_field is not None:
            self.fan_field.setText("—")
        self._fan_is_nc = None
        if self.fan_status_field is not None:
            self.fan_status_field.setText("—")
        self._operation_active = False

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

    def on_auto_toggled(self, enabled: bool) -> None:
        if enabled:
            if not self.model.set_auto_enabled(True):
                self._apply_ui()
                return
            self._apply_ui()
            self._start_action(Action.AUTO_CONNECT, self.worker.auto_connect_saved(), use_timeout=False)
        else:
            self.worker.stop_auto()
            self.model.set_auto_enabled(False)
            self.found_list.blockSignals(True)
            self.found_list.clearSelection()
            self.found_list.blockSignals(False)
            if self.model.state.conn == ConnState.CONNECTED:
                self._lock_selection_to_connected()
            else:
                self.paired_list.blockSignals(True)
                self.paired_list.clearSelection()
                self.paired_list.blockSignals(False)
                self.model.set_selection(None, None)
            fut = self._action_futures.pop(Action.AUTO_CONNECT, None)
            if fut and not fut.done():
                fut.cancel()
            if self.model.state.conn != ConnState.DISCONNECTED:
                self._dispatch_action(Action.DISCONNECT)
                return
            self._apply_ui()


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
