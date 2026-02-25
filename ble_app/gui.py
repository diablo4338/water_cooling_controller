import asyncio
import sys
import time
from typing import Optional

from PySide6.QtCore import QThread, Qt, Signal, Slot, QTimer
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from .core import (
    BleAppCore,
    DeviceInfo,
    add_or_update_paired,
    decode_metrics,
    load_paired_records,
    save_paired_records,
    update_paired_last_connected,
)

APP_TITLE = "BLE Pairing GUI"
DEFAULT_TIMEOUT = 5.0
ACTION_TIMEOUTS = {
    "scan": 7.0,
    "pair": 12.0,
    "connect": 10.0,
    "disconnect": 5.0,
}


class BleWorker(QThread):
    log = Signal(str)
    scan_results = Signal(list)
    data_received = Signal(str)
    pairing_result = Signal(bool, str, object, object)
    connection_state = Signal(bool, object)

    def __init__(self) -> None:
        super().__init__()
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.core = BleAppCore(log=self.log.emit)
        self.device: Optional[DeviceInfo] = None
        self._disconnect_evt: Optional[asyncio.Event] = None
        self._auto_stop_evt: Optional[asyncio.Event] = None
        self._auto_running = False

    def run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def stop(self) -> None:
        if self.loop:
            self.loop.call_soon_threadsafe(self.loop.stop)

    def stop_auto(self) -> None:
        if self.loop and self._auto_stop_evt is not None:
            self.loop.call_soon_threadsafe(self._auto_stop_evt.set)

    def submit(self, coro):
        if not self.loop:
            return None
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    async def _with_timeout(self, coro, label: str, timeout: float = DEFAULT_TIMEOUT):
        try:
            return await asyncio.wait_for(coro, timeout=timeout)
        except asyncio.TimeoutError as exc:
            raise RuntimeError(f"Timeout: {label}") from exc

    def _on_disconnected(self, _) -> None:
        if self._disconnect_evt is not None:
            self._disconnect_evt.set()
        self.connection_state.emit(False, None)
        self.log.emit("Соединение разорвано устройством.")

    async def scan(self) -> None:
        self.log.emit("Поиск устройств в режиме сопряжения...")
        try:
            devices = await self.core.scan_pairing(timeout=5.0)
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
                    await self.connect(device)
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
        await asyncio.wait(
            [self._disconnect_evt.wait(), self._auto_stop_evt.wait()],
            return_when=asyncio.FIRST_COMPLETED,
        )
        if self._auto_stop_evt.is_set():
            await self.disconnect()

    def _load_saved_devices(self) -> list[DeviceInfo]:
        devices: list[DeviceInfo] = []
        records = load_paired_records()
        records.sort(key=lambda r: r.get("last_connected", 0), reverse=True)
        for item in records:
            name = item.get("name", "Unknown")
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=name, address=address))
        return devices

    async def connect(self, device: DeviceInfo) -> None:
        await self.disconnect()
        self.log.emit(f"Подключение к {device.name} ({device.address})...")
        last_exc: Optional[Exception] = None
        self._disconnect_evt = asyncio.Event()
        for attempt in range(1, 2):
            self.log.emit(f"Connect attempt {attempt} to {device.address}")
            try:
                await self.core.connect_raw(device, connect_timeout=2.0)
                if self.core.client and hasattr(self.core.client, "set_disconnected_callback"):
                    self.core.client.set_disconnected_callback(self._on_disconnected)
                self.log.emit("Connected, starting AUTH...")
                await self._do_auth(device)
                self.log.emit("AUTH ok, starting notify...")
                try:
                    await self._with_timeout(
                        self.core.start_metrics_notify(self._on_data),
                        f"start_notify#{attempt}",
                    )
                    self.log.emit("Notify METRICS started.")
                except Exception as exc:
                    self.log.emit(f"Notify METRICS недоступен: {exc}")
                self.device = device
                self.connection_state.emit(True, device)
                self.log.emit("Подключено.")
                return
            except Exception as exc:
                last_exc = exc
                self.log.emit(f"Connect attempt {attempt} failed: {exc}")
                try:
                    await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
                except Exception:
                    pass
                await asyncio.sleep(0.4 * attempt)
        self.log.emit(f"Ошибка подключения: {last_exc}")
        self.connection_state.emit(False, None)

    async def disconnect(self) -> None:
        if self.core.client:
            self.log.emit("Отключение...")
            try:
                self.log.emit("Stopping notify METRICS...")
                await self._with_timeout(self.core.stop_metrics_notify(), "stop_notify", timeout=3.0)
            except Exception:
                pass
            try:
                self.log.emit("Disconnecting BLE...")
                await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
            except Exception:
                pass
            self.log.emit("Отключено.")
        self.device = None
        if self._disconnect_evt is not None:
            self._disconnect_evt.set()
        self.connection_state.emit(False, None)

    async def pair(self, device: DeviceInfo) -> None:
        result = await self.core.pair(device)
        if result.ok:
            self.pairing_result.emit(True, "Устройство спарено", device, result.k_hex)
            return
        self.pairing_result.emit(False, f"Ошибка спаривания: {result.message}", device, None)

    async def _do_auth(self, device: DeviceInfo) -> None:
        await self.core.auth()

    def _on_data(self, data: bytearray) -> None:
        text = decode_metrics(bytes(data))
        self.data_received.emit(text)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(900, 600)

        self.worker = BleWorker()
        self.worker.start()

        self.scan_button = QPushButton("Сканировать")
        self.pair_button = QPushButton("Спарить")
        self.connect_button = QPushButton("Подключить")
        self.disconnect_button = QPushButton("Отключить")
        self.paired_button = QPushButton("Показать спаренные")
        self.delete_button = QPushButton("Удалить сохраненное")
        self.auto_checkbox = QCheckBox("Автоподключение (сохраненные)")
        self._action_buttons = [
            self.scan_button,
            self.pair_button,
            self.connect_button,
            self.disconnect_button,
            self.paired_button,
            self.delete_button,
            self.auto_checkbox,
        ]
        self._action_timers: dict[str, QTimer] = {}
        self._action_futures = {}
        self._active_action: Optional[str] = None
        self._connected = False
        self._connected_device: Optional[DeviceInfo] = None
        self._auto_enabled = False

        self.found_list = QListWidget()
        self.paired_list = QListWidget()
        self.data_view = QTextEdit()
        self.data_view.setReadOnly(True)
        self.status_label = QLabel("Готово")

        buttons = QHBoxLayout()
        buttons.addWidget(self.scan_button)
        buttons.addWidget(self.pair_button)
        buttons.addWidget(self.connect_button)
        buttons.addWidget(self.disconnect_button)
        buttons.addWidget(self.paired_button)
        buttons.addWidget(self.delete_button)
        buttons.addWidget(self.auto_checkbox)

        lists_layout = QHBoxLayout()
        lists_layout.addWidget(self._wrap_list("Устройства для сопряжения", self.found_list))
        lists_layout.addWidget(self._wrap_list("Спаренные устройства", self.paired_list))

        layout = QVBoxLayout()
        layout.addLayout(buttons)
        layout.addLayout(lists_layout)
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
        self.paired_button.clicked.connect(self.on_show_paired)
        self.delete_button.clicked.connect(self.on_delete_paired)
        self.auto_checkbox.toggled.connect(self.on_auto_toggled)

        self.worker.log.connect(self.on_log)
        self.worker.scan_results.connect(self.on_scan_results)
        self.worker.data_received.connect(self.on_data_received)
        self.worker.pairing_result.connect(self.on_pairing_result)
        self.worker.connection_state.connect(self.on_connection_state)

        self.found_list.itemSelectionChanged.connect(self._on_found_selected)
        self.paired_list.itemSelectionChanged.connect(self._on_paired_selected)

        self.on_show_paired()
        self._update_button_states()

    def closeEvent(self, event) -> None:
        self.worker.stop_auto()
        fut = self.worker.submit(self.worker.disconnect())
        if fut is not None:
            try:
                fut.result(timeout=3)
            except Exception:
                pass
        self.worker.stop()
        super().closeEvent(event)

    def _wrap_list(self, title: str, widget: QListWidget) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        layout.addWidget(QLabel(title))
        layout.addWidget(widget)
        wrapper.setLayout(layout)
        return wrapper

    def _selected_device(self) -> Optional[DeviceInfo]:
        item = self.found_list.currentItem()
        if item is None:
            item = self.paired_list.currentItem()
        if item is None:
            return None
        return item.data(Qt.UserRole)

    def _update_button_states(self) -> None:
        selected = self._selected_device()
        has_selection = selected is not None
        self.scan_button.setEnabled(self._active_action is None)
        self.pair_button.setEnabled(self._active_action is None and has_selection)
        self.connect_button.setEnabled(self._active_action is None and has_selection)
        self.disconnect_button.setEnabled(self._active_action is None and self._connected)
        self.paired_button.setEnabled(self._active_action is None)
        self.delete_button.setEnabled(self._active_action is None and self.paired_list.currentItem() is not None)
        self.auto_checkbox.setEnabled(self._active_action is None)

    def _on_found_selected(self) -> None:
        if self.found_list.currentItem() is not None:
            self.paired_list.blockSignals(True)
            self.paired_list.clearSelection()
            self.paired_list.blockSignals(False)
        self._update_button_states()

    def _on_paired_selected(self) -> None:
        if self.paired_list.currentItem() is not None:
            self.found_list.blockSignals(True)
            self.found_list.clearSelection()
            self.found_list.blockSignals(False)
        self._update_button_states()

    def _load_paired(self) -> list[DeviceInfo]:
        raw = load_paired_records()
        devices = []
        for item in raw:
            name = item.get("name", "Unknown")
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=name, address=address))
        return devices

    def _remove_paired(self, address: str) -> bool:
        raw = load_paired_records()
        new_raw = [item for item in raw if item.get("address") != address]
        if len(new_raw) == len(raw):
            return False
        save_paired_records(new_raw)
        return True

    def _add_paired(self, device: DeviceInfo, k_hex: str) -> None:
        add_or_update_paired(device, k_hex)

    def _start_action(
        self,
        name: str,
        coro,
        timeout_override: Optional[float] = None,
        use_timeout: bool = True,
    ) -> None:
        if self._active_action:
            self.on_log(f"Операция '{self._active_action}' уже выполняется.")
            return
        timeout_s = timeout_override if timeout_override is not None else ACTION_TIMEOUTS.get(name, DEFAULT_TIMEOUT)
        self._active_action = name
        for btn in self._action_buttons:
            btn.setEnabled(False)
        if use_timeout:
            timer = QTimer(self)
            timer.setSingleShot(True)
            timer.timeout.connect(lambda: self._action_timeout(name))
            timer.start(int(timeout_s * 1000))
            self._action_timers[name] = timer

        fut = self.worker.submit(coro)
        self._action_futures[name] = fut
        if fut is None:
            self._finish_action(name)
            return

    def _action_timeout(self, name: str) -> None:
        fut = self._action_futures.get(name)
        if fut and not fut.done():
            fut.cancel()
        self.on_log(f"Операция '{name}' превышает таймаут.")
        self._finish_action(name)

    def _finish_action(self, name: str) -> None:
        timer = self._action_timers.pop(name, None)
        if timer:
            timer.stop()
        self._active_action = None
        self._update_button_states()

    def on_scan(self) -> None:
        self._start_action("scan", self.worker.scan())

    def on_pair(self) -> None:
        device = self._selected_device()
        if not device:
            self.on_log("Устройство не выбрано")
            return
        self._start_action("pair", self.worker.pair(device))

    def on_connect(self) -> None:
        device = self._selected_device()
        if not device:
            self.on_log("Устройство не выбрано")
            return
        self._start_action("connect", self.worker.connect(device), timeout_override=ACTION_TIMEOUTS["connect"])

    def on_disconnect(self) -> None:
        self._start_action("disconnect", self.worker.disconnect(), timeout_override=ACTION_TIMEOUTS["disconnect"])

    def on_show_paired(self) -> None:
        self.paired_list.clear()
        for dev in self._load_paired():
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(Qt.UserRole, dev)
            self.paired_list.addItem(item)
        self._update_button_states()

    def on_delete_paired(self) -> None:
        item = self.paired_list.currentItem()
        if item is None:
            self.on_log("Сначала выберите устройство")
            return
        device = item.data(Qt.UserRole)
        if device and self._remove_paired(device.address):
            self.on_show_paired()
            self.on_log(f"Удалено: {device.name}")
        else:
            self.on_log("Не удалось удалить запись.")

    @Slot(list)
    def on_scan_results(self, devices: list) -> None:
        if self._active_action != "scan":
            return
        self.found_list.clear()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(Qt.UserRole, dev)
            self.found_list.addItem(item)
        self._update_button_states()
        if self._active_action == "scan":
            self._finish_action("scan")

    @Slot(str)
    def on_log(self, message: str) -> None:
        self.status_label.setText(message)
        print(message, flush=True)
        if self._connected and self._connected_device:
            self.status_label.setText(f"{message} | {self._connected_device.name}")

    @Slot(str)
    def on_data_received(self, message: str) -> None:
        self.data_view.append(message.rstrip())

    @Slot(bool, str, object, object)
    def on_pairing_result(self, ok: bool, message: str, device: DeviceInfo, k_hex: Optional[str]) -> None:
        if ok:
            if k_hex:
                self._add_paired(device, k_hex)
            self.on_show_paired()
            self.found_list.clear()
        self.on_log(message)
        self._update_button_states()
        if self._active_action == "pair":
            self._finish_action("pair")

    @Slot(bool, object)
    def on_connection_state(self, connected: bool, device: Optional[DeviceInfo]) -> None:
        if connected and device:
            self.on_log(f"Подключено к {device.name}")
            self._connected = True
            self._connected_device = device
            update_paired_last_connected(device.address)
            self._select_paired_device(device)
            if self._active_action == "connect":
                self._finish_action("connect")
        elif not connected:
            self.on_log("Отключено")
            self._connected = False
            self._connected_device = None
            if self._active_action == "disconnect":
                self._finish_action("disconnect")
            elif self._active_action == "connect":
                self._finish_action("connect")
        self._update_button_states()

    def _select_paired_device(self, device: DeviceInfo) -> None:
        for idx in range(self.paired_list.count()):
            item = self.paired_list.item(idx)
            info = item.data(Qt.UserRole)
            if info and getattr(info, "address", None) == device.address:
                self.paired_list.setCurrentItem(item)
                self.paired_list.scrollToItem(item)
                return

    def on_auto_toggled(self, enabled: bool) -> None:
        if enabled:
            self._auto_enabled = True
            self._start_action("connect", self.worker.auto_connect_saved(), use_timeout=False)
        else:
            self._auto_enabled = False
            self.worker.stop_auto()
            if self._active_action == "connect":
                self._finish_action("connect")
        self._update_button_states()


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
