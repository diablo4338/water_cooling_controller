import asyncio
import binascii
import json
import os
import sys
from dataclasses import dataclass
from typing import Optional

from PySide6.QtCore import QThread, Qt, Signal, Slot
from PySide6.QtWidgets import (
    QApplication,
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
PAIR_SVC_SHORT = PAIR_SVC_NORM[4:8]


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

    def run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def stop(self) -> None:
        if self.loop:
            self.loop.call_soon_threadsafe(self.loop.stop)

    def submit(self, coro):
        if not self.loop:
            return None
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    async def scan(self) -> None:
        self.log.emit("Поиск устройств в режиме сопряжения...")
        results: dict[str, DeviceInfo] = {}

        def on_detect(device, advertisement_data) -> None:
            uuids = advertisement_data.service_uuids or []
            for raw in uuids:
                norm = normalize_uuid(raw)
                if norm == PAIR_SVC_NORM or norm.endswith(PAIR_SVC_NORM) or norm == PAIR_SVC_SHORT:
                    name = advertisement_data.local_name or device.name or "Unknown"
                    results[device.address] = DeviceInfo(name=name, address=device.address)
                    break

        try:
            async with BleakScanner(detection_callback=on_detect):
                await asyncio.sleep(5.0)
        except Exception as exc:
            self.log.emit(f"Ошибка сканирования: {exc}")
            return

        found = list(results.values())
        self.scan_results.emit(found)
        self.log.emit(f"Найдено устройств готовых к сопряжению: {len(found)}")

    async def connect(self, device: DeviceInfo) -> None:
        await self.disconnect()
        self.log.emit(f"Подключение к {device.name} ({device.address})...")
        client = BleakClient(device.address)
        try:
            await client.connect()
            await self._do_auth(client, device)
            try:
                await client.start_notify(UUID_METRICS, self._on_data)
            except Exception as exc:
                self.log.emit(f"Notify METRICS недоступен: {exc}")
        except Exception as exc:
            self.log.emit(f"Ошибка подключения: {exc}")
            try:
                await client.disconnect()
            except Exception:
                pass
            self.connection_state.emit(False, None)
            return
        self.client = client
        self.device = device
        self.connection_state.emit(True, device)
        self.log.emit("Подключено.")

    async def disconnect(self) -> None:
        if self.client:
            self.log.emit("Отключение...")
            try:
                await self.client.stop_notify(UUID_METRICS)
            except Exception:
                pass
            try:
                await self.client.disconnect()
            except Exception:
                pass
            self.log.emit("Отключено.")
        self.client = None
        self.device = None
        self.connection_state.emit(False, None)

    async def pair(self, device: DeviceInfo) -> None:
        k_hex: Optional[str] = None
        try:
            async with BleakClient(device.address) as client:
                dev_nonce = await client.read_gatt_char(UUID_PAIR_DEV_NONCE)
                dev_pub65 = await client.read_gatt_char(UUID_PAIR_DEV_PUB)
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

                await client.write_gatt_char(UUID_PAIR_HOST_PUB, host_pub65, response=True)
                confirm = hmac_sha256(k, b"confirm" + bytes(dev_nonce))
                await client.write_gatt_char(UUID_PAIR_CONFIRM, confirm, response=True)
                await client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=False)
                await asyncio.sleep(0.3)
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
        nonce = await client.read_gatt_char(UUID_AUTH_NONCE)
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True)

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

        self.worker.log.connect(self.on_log)
        self.worker.scan_results.connect(self.on_scan_results)
        self.worker.data_received.connect(self.on_data_received)
        self.worker.pairing_result.connect(self.on_pairing_result)
        self.worker.connection_state.connect(self.on_connection_state)

        self.on_show_paired()

    def closeEvent(self, event) -> None:
        self.worker.submit(self.worker.disconnect())
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

    def _load_paired(self) -> list[DeviceInfo]:
        raw = load_paired_records()
        devices = []
        for item in raw:
            name = item.get("name", "Unknown")
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=name, address=address))
        return devices

    def _add_paired(self, device: DeviceInfo, k_hex: str) -> None:
        raw = load_paired_records()
        for item in raw:
            if item.get("address") == device.address:
                item["name"] = device.name
                item["k_hex"] = k_hex
                break
        else:
            raw.append({"name": device.name, "address": device.address, "k_hex": k_hex})
        save_paired_records(raw)

    @Slot()
    def on_scan(self) -> None:
        self.worker.submit(self.worker.scan())

    @Slot()
    def on_pair(self) -> None:
        device = self._selected_device()
        if not device:
            self.on_log("Выберите устройство для спаривания.")
            return
        self.worker.submit(self.worker.pair(device))

    @Slot()
    def on_connect(self) -> None:
        device = self.paired_list.currentItem()
        device = device.data(Qt.UserRole) if device else None
        if not device:
            self.on_log("Выберите устройство для подключения.")
            return
        self.worker.submit(self.worker.connect(device))

    @Slot()
    def on_disconnect(self) -> None:
        self.worker.submit(self.worker.disconnect())

    @Slot()
    def on_show_paired(self) -> None:
        self.paired_list.clear()
        for dev in self._load_paired():
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(Qt.UserRole, dev)
            self.paired_list.addItem(item)

    @Slot(list)
    def on_scan_results(self, devices: list) -> None:
        self.found_list.clear()
        for dev in devices:
            item = QListWidgetItem(f"{dev.name} ({dev.address})")
            item.setData(Qt.UserRole, dev)
            self.found_list.addItem(item)

    @Slot(str)
    def on_log(self, message: str) -> None:
        self.status_label.setText(message)

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
        self.on_log(message)

    @Slot(bool, object)
    def on_connection_state(self, connected: bool, device: Optional[DeviceInfo]) -> None:
        if connected and device:
            self.on_log(f"Подключено к {device.name}")
        elif not connected:
            self.on_log("Отключено")


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
