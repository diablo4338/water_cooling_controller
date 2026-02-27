import asyncio
import sys
from concurrent.futures import Future
from typing import Callable, Optional

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
from .presentation import Action, AppModel, ConnState, SelectionSource

APP_TITLE = "BLE Pairing GUI"
DEFAULT_TIMEOUT = 5.0
ACTION_TIMEOUTS = {
    Action.SCAN: 7.0,
    Action.PAIR: 12.0,
    Action.CONNECT: 10.0,
    Action.DISCONNECT: 5.0,
}
USER_ROLE = Qt.ItemDataRole.UserRole


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

    @staticmethod
    async def _with_timeout(coro, label: str, timeout: float = DEFAULT_TIMEOUT):
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
        wait_disconnect = asyncio.create_task(self._disconnect_evt.wait())
        wait_stop = asyncio.create_task(self._auto_stop_evt.wait())
        done, pending = await asyncio.wait(
            {wait_disconnect, wait_stop},
            return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
            task.cancel()
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
                await self.core.connect_raw(device, connect_timeout=2.0)
                if self.core.client and hasattr(self.core.client, "set_disconnected_callback"):
                    self.core.client.set_disconnected_callback(self._on_disconnected)
                self.log.emit("Connected, starting AUTH...")
                await self._do_auth()
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
                # noinspection PyBroadException
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
            # noinspection PyBroadException
            try:
                self.log.emit("Stopping notify METRICS...")
                await self._with_timeout(self.core.stop_metrics_notify(), "stop_notify", timeout=3.0)
            except Exception:
                pass
            # noinspection PyBroadException
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

    async def _do_auth(self) -> None:
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
        self.model = AppModel()
        self.model.set_status("Готово")

        self.scan_button = QPushButton("Сканировать")
        self.pair_button = QPushButton("Спарить")
        self.connect_button = QPushButton("Подключить")
        self.disconnect_button = QPushButton("Отключить")
        self.paired_button = QPushButton("Показать спаренные")
        self.delete_button = QPushButton("Удалить сохраненное")
        self.auto_checkbox = QCheckBox("Автоподключение (сохраненные)")
        self._action_timers: dict[Action, QTimer] = {}
        self._action_futures: dict[Action, Future] = {}

        self.scan_button.setProperty("actionId", Action.SCAN.name)
        self.pair_button.setProperty("actionId", Action.PAIR.name)
        self.connect_button.setProperty("actionId", Action.CONNECT.name)
        self.disconnect_button.setProperty("actionId", Action.DISCONNECT.name)
        self.paired_button.setProperty("actionId", Action.SHOW_PAIRED.name)
        self.delete_button.setProperty("actionId", Action.DELETE_PAIRED.name)
        self.auto_checkbox.setProperty("actionId", Action.AUTO_CONNECT.name)

        self.found_list = QListWidget()
        self.paired_list = QListWidget()
        self.data_view = QTextEdit()
        self.data_view.setReadOnly(True)
        self.status_label = QLabel(self.model.state.status)

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
        self.paired_button.clicked.connect(self.on_show_paired_clicked)
        self.delete_button.clicked.connect(self.on_delete_paired_clicked)
        self.auto_checkbox.toggled.connect(self.on_auto_toggled)

        self.worker.log.connect(self.on_log)
        self.worker.scan_results.connect(self.on_scan_results)
        self.worker.data_received.connect(self.on_data_received)
        self.worker.pairing_result.connect(self.on_pairing_result)
        self.worker.connection_state.connect(self.on_connection_state)

        self.found_list.itemSelectionChanged.connect(self._on_found_selected)
        self.paired_list.itemSelectionChanged.connect(self._on_paired_selected)

        self._refresh_paired_list()
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
        self.paired_button.setEnabled(Action.SHOW_PAIRED in ui.enabled_actions)
        self.delete_button.setEnabled(Action.DELETE_PAIRED in ui.enabled_actions)
        self.auto_checkbox.setEnabled(Action.AUTO_CONNECT in ui.enabled_actions)

        self.auto_checkbox.blockSignals(True)
        self.auto_checkbox.setChecked(ui.auto_enabled)
        self.auto_checkbox.blockSignals(False)

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
            timeout_override if timeout_override is not None else ACTION_TIMEOUTS.get(action, DEFAULT_TIMEOUT)
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
        self.on_log(f"Операция '{action.name}' превышает таймаут.")
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
                timeout_override=ACTION_TIMEOUTS[Action.CONNECT],
            )
        elif command.action == Action.DISCONNECT:
            self._start_action(
                Action.DISCONNECT,
                self.worker.disconnect(),
                timeout_override=ACTION_TIMEOUTS[Action.DISCONNECT],
            )
        elif command.action == Action.SHOW_PAIRED:
            self._refresh_paired_list()
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

    def on_show_paired_clicked(self) -> None:
        self._dispatch_action(Action.SHOW_PAIRED)

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

    @Slot(str)
    def on_data_received(self, message: str) -> None:
        self.data_view.append(message.rstrip())

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
            self._select_paired_device(device)
            if self.model.state.active_action == Action.CONNECT:
                self._finish_action(Action.CONNECT)
            self.on_log(f"Подключено к {device.name}")
        elif not connected:
            self.model.set_connected(False, None)
            if self.model.state.active_action == Action.DISCONNECT:
                self._finish_action(Action.DISCONNECT)
            elif self.model.state.active_action == Action.CONNECT:
                self._finish_action(Action.CONNECT)
            self.on_log("Отключено")

    def _select_paired_device(self, device: DeviceInfo) -> None:
        for idx in range(self.paired_list.count()):
            item = self.paired_list.item(idx)
            info = item.data(USER_ROLE)
            if info and getattr(info, "address", None) == device.address:
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
            self.paired_list.blockSignals(True)
            self.found_list.clearSelection()
            self.paired_list.clearSelection()
            self.found_list.blockSignals(False)
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
