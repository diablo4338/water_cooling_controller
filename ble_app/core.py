import asyncio
import binascii
import json
import os
import time
from dataclasses import dataclass
from typing import Callable, Optional

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

# PUBLIC API: used by GUI and tests. Do not change behavior without updating tests.
__all__ = [
    "UUID_PAIR_SVC",
    "UUID_MAIN_SVC",
    "UUID_PAIR_DEV_NONCE",
    "UUID_PAIR_DEV_PUB",
    "UUID_PAIR_HOST_PUB",
    "UUID_PAIR_CONFIRM",
    "UUID_PAIR_FINISH",
    "UUID_AUTH_NONCE",
    "UUID_AUTH_PROOF",
    "UUID_METRICS",
    "UUID_CMD",
    "PAIR_SVC_NORM",
    "PAIRED_DB",
    "HOST_KEY_PATH",
    "DeviceInfo",
    "PairResult",
    "normalize_uuid",
    "hmac_sha256",
    "derive_k",
    "load_or_create_host_key",
    "decode_metrics",
    "load_paired_records",
    "save_paired_records",
    "find_paired_record",
    "update_paired_last_connected",
    "add_or_update_paired",
    "BleAppCore",
]

# Public BLE protocol constants used by app and tests.
UUID_PAIR_SVC = "8fdd08d6-2a9e-4d5a-9f44-9f58b3a9d3c1"
UUID_MAIN_SVC = "3d1a4b35-9707-43e6-bf3e-2e2f7b561d82"

UUID_PAIR_DEV_NONCE = "0b46b3cf-7e3b-44a3-8f39-4af2a8c9a1ee"
UUID_PAIR_DEV_PUB = "91c66f66-5c92-4c4d-86bf-6d2c58b6f0d7"
UUID_PAIR_HOST_PUB = "c9c8f69a-1f49-4ea0-a0a2-3c0d0a69e9d4"
UUID_PAIR_CONFIRM = "f5ee9c0b-96ae-4dc0-9b46-5f6f7f2ad2bf"
UUID_PAIR_FINISH = "a4c8e2c1-1c7b-4b06-a59f-4b5f8a2a8b3c"

UUID_AUTH_NONCE = "f1d1f9b6-8c92-47f6-a2f5-5b0a77d2e3a9"
UUID_AUTH_PROOF = "74cde77a-7f14-4e6e-b7f5-92ef0c3ad7e4"

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


def add_or_update_paired(device: "DeviceInfo", k_hex: str) -> None:
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


@dataclass(frozen=True)
class DeviceInfo:
    name: str
    address: str


@dataclass(frozen=True)
class PairResult:
    ok: bool
    message: str
    k_hex: Optional[str]


class BleAppCore:
    """
    PUBLIC API used by both the GUI and integration tests.
    Do not change behavior without adjusting tests.
    """

    def __init__(self, log: Optional[Callable[[str], None]] = None, adapter: Optional[str] = None):
        self._log = log
        self._adapter = adapter
        self._host_key = load_or_create_host_key()
        self.client: Optional[BleakClient] = None
        self.device: Optional[DeviceInfo] = None

    def set_adapter(self, adapter: Optional[str]) -> None:
        self._adapter = adapter

    def _emit(self, msg: str) -> None:
        if self._log:
            self._log(msg)

    def _scanner_kwargs(self) -> dict:
        if self._adapter:
            return {"bluez": {"adapter": self._adapter}}
        return {}

    async def scan_pairing(self, timeout: float = 5.0) -> list[DeviceInfo]:
        results: dict[str, DeviceInfo] = {}

        def on_detect(device, advertisement_data) -> None:
            uuids = advertisement_data.service_uuids or []
            norm_uuids = {normalize_uuid(raw) for raw in uuids}
            name = advertisement_data.local_name or device.name or "Unknown"
            if PAIR_SVC_NORM in norm_uuids:
                results[device.address] = DeviceInfo(name=name, address=device.address)
                return

        async with BleakScanner(detection_callback=on_detect, **self._scanner_kwargs()):
            await asyncio.sleep(timeout)

        return list(results.values())

    async def resolve_device(self, address: str, timeout: float = 5.0) -> DeviceInfo:
        dev = await BleakScanner.find_device_by_address(
            address, timeout=timeout, **self._scanner_kwargs()
        )
        if not dev:
            raise RuntimeError(f"Device with address {address} was not found.")
        name = dev.name or "Unknown"
        return DeviceInfo(name=name, address=dev.address)

    async def connect_raw(self, device: DeviceInfo, connect_timeout: float = 5.0) -> None:
        await self.disconnect()
        client = BleakClient(device.address, adapter=self._adapter)
        await asyncio.wait_for(client.connect(), timeout=connect_timeout)
        self.client = client
        self.device = device

    async def disconnect(self) -> None:
        if self.client:
            try:
                await self.client.disconnect()
            except Exception:
                pass
        self.client = None
        self.device = None

    async def pair(self, device: DeviceInfo, timeout: float = 8.0) -> PairResult:
        k_hex: Optional[str] = None
        client = BleakClient(device.address, adapter=self._adapter)
        try:
            async with client:
                dev_nonce = await asyncio.wait_for(
                    client.read_gatt_char(UUID_PAIR_DEV_NONCE), timeout=timeout
                )
                dev_pub65 = await asyncio.wait_for(
                    client.read_gatt_char(UUID_PAIR_DEV_PUB), timeout=timeout
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

                await asyncio.wait_for(
                    client.write_gatt_char(UUID_PAIR_HOST_PUB, host_pub65, response=True),
                    timeout=timeout,
                )
                confirm = hmac_sha256(k, b"confirm" + bytes(dev_nonce))
                await asyncio.wait_for(
                    client.write_gatt_char(UUID_PAIR_CONFIRM, confirm, response=True),
                    timeout=timeout,
                )
                await asyncio.wait_for(
                    client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=True),
                    timeout=timeout,
                )
        except Exception as exc:
            return PairResult(False, f"Pairing failed: {exc}", None)
        return PairResult(True, "Paired", k_hex)

    async def auth(self, timeout: float = 5.0) -> None:
        if not self.client or not self.device:
            raise RuntimeError("Not connected")
        record = find_paired_record(self.device.address)
        if not record:
            raise RuntimeError("Device not paired")
        if "k_hex" not in record:
            raise RuntimeError("Missing K")
        k = binascii.unhexlify(record["k_hex"])
        nonce = await asyncio.wait_for(self.client.read_gatt_char(UUID_AUTH_NONCE), timeout=timeout)
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await asyncio.wait_for(
            self.client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True),
            timeout=timeout,
        )

    async def read_metrics(self, timeout: float = 5.0) -> bytes:
        if not self.client:
            raise RuntimeError("Not connected")
        return await asyncio.wait_for(self.client.read_gatt_char(UUID_METRICS), timeout=timeout)

    async def start_metrics_notify(self, callback: Callable[[bytearray], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.start_notify(UUID_METRICS, lambda _, data: callback(data))

    async def stop_metrics_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.stop_notify(UUID_METRICS)
