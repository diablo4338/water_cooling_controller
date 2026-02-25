import asyncio
import binascii
import json
import os
import sys
import time
from dataclasses import dataclass
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

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.serialization import (
    Encoding,
    NoEncryption,
    PrivateFormat,
    PublicFormat,
    load_pem_private_key,
)

APP_TITLE = "BLE Pairing GUI"
DEFAULT_TIMEOUT = 5.0
ACTION_TIMEOUTS = {
    "scan": 7.0,
    "pair": 12.0,
    "connect": 10.0,
    "disconnect": 5.0,
}

UUID_PAIR_SVC = "8fdd08d6-2a9e-4d5a-9f44-9f58b3a9d3c1"
UUID_MAIN_SVC = "3d1a4b35-9707-43e6-bf3e-2e2f7b561d82"

UUID_PAIR_DEV_NONCE = "0b46b3cf-7e3b-44a3-8f39-4af2a8c9a1ee"
UUID_PAIR_DEV_PUB = "91c66f66-5c92-4c4d-86bf-6d2c58b6f0d7"
UUID_PAIR_HOST_PUB = "c9c8f69a-1f49-4ea0-a0a2-3c0d0a69e9d4"
UUID_PAIR_CONFIRM = "f5ee9c0b-96ae-4dc0-9b46-5f6f7f2ad2bf"
UUID_PAIR_FINISH = "a4c8e2c1-1c7b-4b06-a59f-4b5f8a2a8b3c"

UUID_AUTH_NONCE = "f1d1f9b6-8c92-47f6-a2f5-5b0a77d2e3a9"
UUID_AUTH_PROOF = "74cde77a-7f14-4e6e-b7f5-92ef0c3ad7e4"

# Эти UUID нужно синхронизировать с прошивкой устройства.
UUID_METRICS = "a9b66c3d-3a6e-4b75-8b67-1dfbdb2a7e11"
UUID_CMD = "3d1a4b35-9707-43e6-bf3e-2e2f7b561d84"

PAIRED_DB = "paired_devices.json"
HOST_KEY_PATH = "host_key.pem"


def normalize_uuid(value: str) -> str:
    return value.replace("-", "").lower()


PAIR_SVC_NORM = normalize_uuid(UUID_PAIR_SVC)

def hmac_sha256(key: bytes, msg: bytes) -> bytes:
    h = hmac.HMAC(key, hashes.SHA256())
    h.update(msg)
    return h.finalize()


def derive_k(shared_secret: bytes, dev_nonce: bytes) -> bytes:
    hkdf = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=dev_nonce,
        info=b"PAIRv1",
    )
    return hkdf.derive(shared_secret)


def load_or_create_host_key() -> ec.EllipticCurvePrivateKey:
    if os.path.exists(HOST_KEY_PATH):
        with open(HOST_KEY_PATH, "rb") as f:
            return load_pem_private_key(f.read(), password=None)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption())
    with open(HOST_KEY_PATH, "wb") as f:
        f.write(pem)
    return key


def decode_metrics(data: bytes) -> str:
    try:
        text = data.decode("utf-8")
        printable = all(ch.isprintable() or ch in "\r\n\t" for ch in text)
        if printable:
            return text
    except Exception:
        pass
    return "0x" + binascii.hexlify(data).decode()


def load_paired_records() -> list[dict]:
    if not os.path.exists(PAIRED_DB):
        return []
    try:
        with open(PAIRED_DB, "r", encoding="utf-8") as f:
            raw = json.load(f)
        if isinstance(raw, list):
            return raw
    except Exception:
        return []
    return []


def save_paired_records(records: list[dict]) -> None:
    with open(PAIRED_DB, "w", encoding="utf-8") as f:
        json.dump(records, f, ensure_ascii=False, indent=2)


def find_paired_record(address: str) -> Optional[dict]:
    for item in load_paired_records():
        if item.get("address") == address:
            return item
    return None


def update_paired_last_connected(address: str) -> None:
    raw = load_paired_records()
    now = int(time.time())
    changed = False
    for item in raw:
        if item.get("address") == address:
            item["last_connected"] = now
            changed = True
            break
    if changed:
        save_paired_records(raw)


@dataclass
class DeviceInfo:
    name: str
    address: str


class BleWorker(QThread):
    log = Signal(str)
    scan_results = Signal(list)
    data_received = Signal(str)
    pairing_result = Signal(bool, str, object, object)
    connection_state = Signal(bool, object)

    def __init__(self) -> None:
        super().__init__()
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.client: Optional[BleakClient] = None
        self.device: Optional[DeviceInfo] = None
        self._host_key = load_or_create_host_key()
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

    def _on_disconnected(self, _: BleakClient) -> None:
        if self._disconnect_evt is not None:
            self._disconnect_evt.set()
        self.connection_state.emit(False, None)
        self.log.emit("Соединение разорвано устройством.")

    async def scan(self) -> None:
        self.log.emit("Поиск устройств в режиме сопряжения...")
        results: dict[str, DeviceInfo] = {}

        def on_detect(device, advertisement_data) -> None:
            uuids = advertisement_data.service_uuids or []
            norm_uuids = {normalize_uuid(raw) for raw in uuids}
            name = advertisement_data.local_name or device.name or "Unknown"
            if PAIR_SVC_NORM in norm_uuids:
                results[device.address] = DeviceInfo(name=name, address=device.address)
                return

        try:
            async with BleakScanner(detection_callback=on_detect):
                await asyncio.sleep(5.0)
        except Exception as exc:
            self.log.emit(f"Ошибка сканирования: {exc}")
            self.scan_results.emit([])
            return

        found = list(results.values())
        self.scan_results.emit(found)
        self.log.emit(f"Найдено устройств готовых к сопряжению: {len(found)}")

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
                    if self.client:
                        await self._wait_disconnect_or_stop()
                    if self._auto_stop_evt.is_set():
                        break
        finally:
            self._auto_running = False
            self._auto_stop_evt = None

    async def _wait_disconnect_or_stop(self) -> None:
        if self._disconnect_evt is None or self._auto_stop_evt is None:
            return
        done, _ = await asyncio.wait(
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
            client = BleakClient(device.address)
            if hasattr(client, "set_disconnected_callback"):
                client.set_disconnected_callback(self._on_disconnected)
            else:
                client = BleakClient(device.address, disconnected_callback=self._on_disconnected)
            try:
                await self._with_timeout(client.connect(), f"connect#{attempt}", timeout=2.0)
                self.log.emit("Connected, starting AUTH...")
                await self._do_auth(client, device)
                self.log.emit("AUTH ok, starting notify...")
                try:
                    await self._with_timeout(
                        client.start_notify(UUID_METRICS, self._on_data),
                        f"start_notify#{attempt}",
                    )
                    self.log.emit("Notify METRICS started.")
                except Exception as exc:
                    self.log.emit(f"Notify METRICS недоступен: {exc}")
                self.client = client
                self.device = device
                self.connection_state.emit(True, device)
                self.log.emit("Подключено.")
                return
            except Exception as exc:
                last_exc = exc
                self.log.emit(f"Connect attempt {attempt} failed: {exc}")
                try:
                    await self._with_timeout(client.disconnect(), "disconnect", timeout=3.0)
                except Exception:
                    pass
                await asyncio.sleep(0.4 * attempt)
        self.log.emit(f"Ошибка подключения: {last_exc}")
        self.connection_state.emit(False, None)

    async def disconnect(self) -> None:
        if self.client:
            self.log.emit("Отключение...")
            try:
                self.log.emit("Stopping notify METRICS...")
                await self._with_timeout(self.client.stop_notify(UUID_METRICS), "stop_notify", timeout=3.0)
            except Exception:
                pass
            try:
                self.log.emit("Disconnecting BLE...")
                await self._with_timeout(self.client.disconnect(), "disconnect", timeout=3.0)
            except Exception:
                pass
            self.log.emit("Отключено.")
        self.client = None
        self.device = None
        if self._disconnect_evt is not None:
            self._disconnect_evt.set()
        self.connection_state.emit(False, None)

    async def pair(self, device: DeviceInfo) -> None:
        k_hex: Optional[str] = None
        try:
            client = BleakClient(device.address)
            if hasattr(client, "set_disconnected_callback"):
                client.set_disconnected_callback(self._on_disconnected)
            else:
                client = BleakClient(device.address, disconnected_callback=self._on_disconnected)
            async with client:
                self._disconnect_evt = asyncio.Event()
                dev_nonce = await self._with_timeout(
                    client.read_gatt_char(UUID_PAIR_DEV_NONCE), "read_dev_nonce"
                )
                dev_pub65 = await self._with_timeout(
                    client.read_gatt_char(UUID_PAIR_DEV_PUB), "read_dev_pub"
                )
                if len(dev_nonce) != 16:
                    raise RuntimeError(f"Bad dev_nonce len={len(dev_nonce)}")
                if len(dev_pub65) != 65 or dev_pub65[0] != 0x04:
                    raise RuntimeError("Bad dev_pub format (expected 65 bytes uncompressed P-256)")

                host_pub65 = self._host_key.public_key().public_bytes(
                    Encoding.X962, PublicFormat.UncompressedPoint
                )

                dev_pub_obj = ec.EllipticCurvePublicKey.from_encoded_point(
                    ec.SECP256R1(), bytes(dev_pub65)
                )
                shared = self._host_key.exchange(ec.ECDH(), dev_pub_obj)
                k = derive_k(shared, bytes(dev_nonce))
                k_hex = binascii.hexlify(k).decode()

                await self._with_timeout(
                    client.write_gatt_char(UUID_PAIR_HOST_PUB, host_pub65, response=True),
                    "write_host_pub",
                )
                confirm = hmac_sha256(k, b"confirm" + bytes(dev_nonce))
                await self._with_timeout(
                    client.write_gatt_char(UUID_PAIR_CONFIRM, confirm, response=True),
                    "write_pair_confirm",
                )
                await self._with_timeout(
                    client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=True),
                    "write_pair_finish",
                )
                try:
                    await asyncio.wait_for(self._disconnect_evt.wait(), timeout=2.0)
                except asyncio.TimeoutError:
                    pass
                self._disconnect_evt = None
        except Exception as exc:
            print(exc)
            self.pairing_result.emit(False, f"Ошибка спаривания: {exc}", device, None)
            return
        self.pairing_result.emit(True, "Устройство спарено", device, k_hex)

    async def _do_auth(self, client: BleakClient, device: DeviceInfo) -> None:
        record = find_paired_record(device.address)
        if not record:
            raise RuntimeError("Устройство не спарено. Сначала выполните спаривание.")
        if "k_hex" not in record:
            raise RuntimeError("Нет ключа K для устройства.")
        k = binascii.unhexlify(record["k_hex"])
        nonce = await self._with_timeout(client.read_gatt_char(UUID_AUTH_NONCE), "read_auth_nonce")
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await self._with_timeout(
            client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True),
            "write_auth_proof",
        )

    def _on_data(self, _: int, data: bytearray) -> None:
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
        if self._auto_enabled:
            for btn in self._action_buttons:
                btn.setEnabled(False)
            self.auto_checkbox.setEnabled(True)
            self.found_list.setEnabled(False)
            self.paired_list.setEnabled(False)
            return
        if self._active_action:
            return
        self.found_list.setEnabled(True)
        self.paired_list.setEnabled(True)
        self.scan_button.setEnabled(True)
        found_selected = self.found_list.currentItem() is not None
        paired_selected = self.paired_list.currentItem() is not None
        self.pair_button.setEnabled(found_selected)
        self.connect_button.setEnabled(paired_selected)
        self.disconnect_button.setEnabled(self._connected)
        self.delete_button.setEnabled(paired_selected)

    @Slot(bool)
    def on_auto_toggled(self, enabled: bool) -> None:
        self._auto_enabled = enabled
        if enabled:
            self.on_log("Автоподключение включено.")
            self._start_action("auto_connect", self.worker.auto_connect_saved(), use_timeout=False)
        else:
            self.on_log("Автоподключение выключено.")
            self.worker.stop_auto()
            if self._active_action == "auto_connect":
                fut = self._action_futures.get(self._active_action)
                if fut and not fut.done():
                    fut.cancel()
                self._finish_action(self._active_action)
            self._start_action("disconnect", self.worker.disconnect())
        self._update_button_states()

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
        raw = load_paired_records()
        for item in raw:
            if item.get("address") == device.address:
                item["name"] = device.name
                item["k_hex"] = k_hex
                if "last_connected" not in item:
                    item["last_connected"] = 0
                break
        else:
            raw.append(
                {
                    "name": device.name,
                    "address": device.address,
                    "k_hex": k_hex,
                    "last_connected": 0,
                }
            )
        save_paired_records(raw)

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
        self.on_log(f"Таймаут операции '{name}'.")
        self._finish_action(name)

    def _finish_action(self, name: str) -> None:
        timer = self._action_timers.pop(name, None)
        if timer:
            timer.stop()
            timer.deleteLater()
        self._action_futures.pop(name, None)
        self._active_action = None
        if not self._auto_enabled:
            for btn in self._action_buttons:
                btn.setEnabled(True)
        self._update_button_states()

    @Slot()
    def on_scan(self) -> None:
        self.found_list.clear()
        self._start_action("scan", self.worker.scan())

    @Slot()
    def on_pair(self) -> None:
        device = self._selected_device()
        if not device:
            self.on_log("Выберите устройство для спаривания.")
            return
        self._start_action("pair", self.worker.pair(device))

    @Slot()
    def on_connect(self) -> None:
        device = self.paired_list.currentItem()
        device = device.data(Qt.UserRole) if device else None
        if not device:
            self.on_log("Выберите устройство для подключения.")
            return
        self._start_action("connect", self.worker.connect(device))

    @Slot()
    def on_disconnect(self) -> None:
        self._start_action("disconnect", self.worker.disconnect())

    @Slot()
    def on_show_paired(self) -> None:
        self.paired_list.clear()
        for dev in self._load_paired():
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(Qt.UserRole, dev)
            self.paired_list.addItem(item)
        self._update_button_states()

    @Slot()
    def on_delete_paired(self) -> None:
        item = self.paired_list.currentItem()
        device = item.data(Qt.UserRole) if item else None
        if not device:
            self.on_log("Выберите устройство для удаления.")
            return
        if self._remove_paired(device.address):
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
    def on_pairing_result(
        self, ok: bool, message: str, device: DeviceInfo, k_hex: Optional[str]
    ) -> None:
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


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
