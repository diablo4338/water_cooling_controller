import asyncio
import binascii
import json
import os
import struct
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
from platformdirs import user_config_dir

from .config import BleConfig, DEFAULT_CONFIG

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
    "UUID_METRICS_SVC",
    "UUID_TEMP0_VALUE",
    "UUID_TEMP1_VALUE",
    "UUID_TEMP2_VALUE",
    "UUID_TEMP3_VALUE",
    "UUID_FAN_SPEED_VALUE",
    "UUID_FAN2_SPEED_VALUE",
    "UUID_FAN3_SPEED_VALUE",
    "UUID_FAN4_SPEED_VALUE",
    "UUID_CONFIG_SVC",
    "UUID_CONFIG_PARAMS",
    "UUID_CONFIG_STATUS",
    "UUID_CONFIG_FAN_STATUS",
    "UUID_CONFIG_DEVICE_STATUS",
    "UUID_OPERATIONS_SVC",
    "UUID_OP_CONTROL",
    "UUID_OP_STATUS",
    "PAIR_SVC_NORM",
    "PAIRED_DB",
    "HOST_KEY_PATH",
    "PARAMS_DB",
    "PARAMS_VERSION",
    "PARAM_STATUS_OK",
    "PARAM_STATUS_INVALID",
    "PARAM_STATUS_BUSY",
    "PARAM_FIELD_NONE",
    "PARAM_FIELD_NAMES",
    "FAN_CONTROL_DC",
    "FAN_CONTROL_PWM",
    "FAN_MODE_CONTINUOUS",
    "FAN_MODE_TEMP_SENSOR",
    "FAN_MODE_INACTIVE",
    "FAN_CONTROL_TYPE_NAMES",
    "FAN_MODE_NAMES",
    "FAN_STATUS_VERSION",
    "DEVICE_STATUS_VERSION",
    "DEVICE_STATE_OK",
    "DEVICE_STATE_ERROR",
    "DEVICE_ERROR_NONE",
    "DEVICE_ERROR_ADC_OFFLINE",
    "DEVICE_STATE_NAMES",
    "DEVICE_ERROR_NAMES",
    "FAN_STATE_IDLE",
    "FAN_STATE_STARTING",
    "FAN_STATE_RUNNING",
    "FAN_STATE_STALL",
    "FAN_STATE_IN_SERVICE",
    "FAN_STATE_NAMES",
    "OP_STATUS_VERSION",
    "OP_CONTROL_VERSION",
    "OP_ERROR_TEXT_MAX",
    "OP_TYPE_NONE",
    "OP_TYPE_FAN_CALIBRATION",
    "OP_TYPE_SETUP_FANS",
    "OP_TYPE_NAMES",
    "OP_STATE_IDLE",
    "OP_STATE_IN_SERVICE",
    "OP_STATE_DONE",
    "OP_STATE_ERROR",
    "OP_STATE_NAMES",
    "DeviceParams",
    "FanStatus",
    "DeviceStatus",
    "OperationStatus",
    "ParamsStatus",
    "DeviceInfo",
    "PairResult",
    "normalize_uuid",
    "hmac_sha256",
    "derive_k",
    "load_or_create_host_key",
    "decode_metrics",
    "decode_temp_value",
    "decode_fan_speed",
    "encode_params",
    "decode_params",
    "decode_params_status",
    "decode_fan_status",
    "decode_device_status",
    "encode_operation_control",
    "decode_operation_status",
    "load_params",
    "load_device_params",
    "save_params",
    "save_device_params",
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

UUID_METRICS_SVC = "f3a0c1d2-5b6a-4d2e-9b43-1c2d3e4f5061"
UUID_TEMP0_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718291"
UUID_TEMP1_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718292"
UUID_TEMP2_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718293"
UUID_TEMP3_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718294"
UUID_FAN_SPEED_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718295"
UUID_FAN2_SPEED_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718296"
UUID_FAN3_SPEED_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718297"
UUID_FAN4_SPEED_VALUE = "a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718298"

UUID_CONFIG_SVC = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e10"
UUID_CONFIG_PARAMS = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e11"
UUID_CONFIG_STATUS = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e12"
UUID_CONFIG_FAN_STATUS = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e13"
UUID_CONFIG_DEVICE_STATUS = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e14"
UUID_OPERATIONS_SVC = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e20"
UUID_OP_CONTROL = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e21"
UUID_OP_STATUS = "6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e22"

TEMP_CHAR_UUIDS = [UUID_TEMP0_VALUE, UUID_TEMP1_VALUE, UUID_TEMP2_VALUE, UUID_TEMP3_VALUE]
FAN_SPEED_UUIDS = [
    UUID_FAN_SPEED_VALUE,
    UUID_FAN2_SPEED_VALUE,
    UUID_FAN3_SPEED_VALUE,
    UUID_FAN4_SPEED_VALUE,
]

APP_NAME = "BLECoolingController"


def _ensure_app_config_dir() -> str:
    path = user_config_dir(APP_NAME, ensure_exists=True)
    os.makedirs(path, exist_ok=True)
    return path


APP_CONFIG_DIR = _ensure_app_config_dir()
PAIRED_DB = os.path.join(APP_CONFIG_DIR, "paired_devices.json")
HOST_KEY_PATH = os.path.join(APP_CONFIG_DIR, "host_key.pem")
PARAMS_DB = os.path.join(APP_CONFIG_DIR, "params.json")

PARAMS_VERSION = 5
PARAM_STATUS_OK = 0
PARAM_STATUS_INVALID = 1
PARAM_STATUS_BUSY = 2
PARAM_FIELD_NONE = 0xFF
PARAM_FIELD_NAMES = {
    0: "fan_min_speed",
    1: "fan_control_type",
    2: "fan_max_temp",
    3: "fan_off_delta",
    4: "fan_start_temp",
    5: "fan_mode",
    6: "fan_monitoring_enabled",
    7: "fan2_monitoring_enabled",
    8: "fan3_monitoring_enabled",
    9: "fan4_monitoring_enabled",
}
FAN_CONTROL_DC = 0
FAN_CONTROL_PWM = 1
FAN_MODE_CONTINUOUS = 0
FAN_MODE_TEMP_SENSOR = 1
FAN_MODE_INACTIVE = 2
FAN_CONTROL_TYPE_NAMES = {
    FAN_CONTROL_DC: "DC",
    FAN_CONTROL_PWM: "PWM",
}
FAN_MODE_NAMES = {
    FAN_MODE_CONTINUOUS: "Always on",
    FAN_MODE_TEMP_SENSOR: "By temperature sensor",
    FAN_MODE_INACTIVE: "Inactive (fans off)",
}
FAN_STATUS_VERSION = 1
DEVICE_STATUS_VERSION = 1
DEVICE_STATE_OK = 0
DEVICE_STATE_ERROR = 1
DEVICE_ERROR_NONE = 0
DEVICE_ERROR_ADC_OFFLINE = 1
DEVICE_STATE_NAMES = {
    DEVICE_STATE_OK: "OK",
    DEVICE_STATE_ERROR: "ERROR",
}
DEVICE_ERROR_NAMES = {
    DEVICE_ERROR_NONE: "NONE",
    DEVICE_ERROR_ADC_OFFLINE: "ADC_OFFLINE",
}
FAN_STATE_IDLE = 0
FAN_STATE_STARTING = 1
FAN_STATE_RUNNING = 2
FAN_STATE_STALL = 3
FAN_STATE_IN_SERVICE = 4
FAN_STATE_NAMES = {
    FAN_STATE_IDLE: "IDLE",
    FAN_STATE_STARTING: "STARTING",
    FAN_STATE_RUNNING: "RUNNING",
    FAN_STATE_STALL: "STALL",
    FAN_STATE_IN_SERVICE: "IN_SERVICE",
}

OP_STATUS_VERSION = 1
OP_CONTROL_VERSION = 1
OP_ERROR_TEXT_MAX = 20
OP_TYPE_NONE = 0
OP_TYPE_FAN_CALIBRATION = 1
OP_TYPE_SETUP_FANS = 2
OP_TYPE_NAMES = {
    OP_TYPE_NONE: "NONE",
    OP_TYPE_FAN_CALIBRATION: "FAN_CALIBRATION",
    OP_TYPE_SETUP_FANS: "SETUP_FANS",
}
OP_STATE_IDLE = 0
OP_STATE_IN_SERVICE = 1
OP_STATE_DONE = 2
OP_STATE_ERROR = 3
OP_STATE_NAMES = {
    OP_STATE_IDLE: "IDLE",
    OP_STATE_IN_SERVICE: "IN_SERVICE",
    OP_STATE_DONE: "DONE",
    OP_STATE_ERROR: "ERROR",
}


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
    value = decode_temp_value(data)
    if value is None:
        return "0x" + binascii.hexlify(data).decode()
    return f"{value:.2f}"


def decode_temp_value(data: bytes) -> Optional[float]:
    if len(data) != 4:
        return None
    return struct.unpack("<f", data)[0]


def decode_fan_speed(data: bytes) -> Optional[float]:
    return decode_temp_value(data)


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
class DeviceParams:
    fan_min_speed: int
    fan_control_type: int
    fan_max_temp: int
    fan_off_delta: int
    fan_start_temp: int
    fan_mode: int
    fan_monitoring_enabled: bool
    fan2_monitoring_enabled: bool
    fan3_monitoring_enabled: bool
    fan4_monitoring_enabled: bool


@dataclass(frozen=True)
class ParamsStatus:
    ok: bool
    status: int
    field_id: Optional[int]


@dataclass(frozen=True)
class FanStatus:
    states: tuple[int, int, int, int]
    labels: tuple[str, str, str, str]
    op_type: int
    op_label: str

    @property
    def state(self) -> int:
        if FAN_STATE_STALL in self.states:
            return FAN_STATE_STALL
        if FAN_STATE_IN_SERVICE in self.states:
            return FAN_STATE_IN_SERVICE
        if FAN_STATE_STARTING in self.states:
            return FAN_STATE_STARTING
        if FAN_STATE_RUNNING in self.states:
            return FAN_STATE_RUNNING
        return FAN_STATE_IDLE

    @property
    def label(self) -> str:
        return FAN_STATE_NAMES.get(self.state, "UNKNOWN")


@dataclass(frozen=True)
class DeviceStatus:
    state: int
    label: str
    error: int
    error_label: str


@dataclass(frozen=True)
class OperationStatus:
    op_type: int
    state: int
    error: str


DEFAULT_PARAMS = DeviceParams(
    fan_min_speed=10,
    fan_control_type=FAN_CONTROL_DC,
    fan_max_temp=45,
    fan_off_delta=2,
    fan_start_temp=35,
    fan_mode=FAN_MODE_CONTINUOUS,
    fan_monitoring_enabled=True,
    fan2_monitoring_enabled=True,
    fan3_monitoring_enabled=True,
    fan4_monitoring_enabled=True,
)


def _params_from_dict(raw: dict) -> Optional[DeviceParams]:
    try:
        return DeviceParams(
            fan_min_speed=int(raw.get("fan_min_speed", DEFAULT_PARAMS.fan_min_speed)),
            fan_control_type=int(raw.get("fan_control_type", DEFAULT_PARAMS.fan_control_type)),
            fan_max_temp=int(raw.get("fan_max_temp", DEFAULT_PARAMS.fan_max_temp)),
            fan_off_delta=int(raw.get("fan_off_delta", DEFAULT_PARAMS.fan_off_delta)),
            fan_start_temp=int(raw.get("fan_start_temp", DEFAULT_PARAMS.fan_start_temp)),
            fan_mode=int(raw.get("fan_mode", DEFAULT_PARAMS.fan_mode)),
            fan_monitoring_enabled=bool(
                raw.get("fan_monitoring_enabled", DEFAULT_PARAMS.fan_monitoring_enabled)
            ),
            fan2_monitoring_enabled=bool(
                raw.get("fan2_monitoring_enabled", DEFAULT_PARAMS.fan2_monitoring_enabled)
            ),
            fan3_monitoring_enabled=bool(
                raw.get("fan3_monitoring_enabled", DEFAULT_PARAMS.fan3_monitoring_enabled)
            ),
            fan4_monitoring_enabled=bool(
                raw.get("fan4_monitoring_enabled", DEFAULT_PARAMS.fan4_monitoring_enabled)
            ),
        )
    except (TypeError, ValueError):
        return None


def _params_to_dict(params: DeviceParams) -> dict:
    return {
        "version": PARAMS_VERSION,
        "fan_min_speed": params.fan_min_speed,
        "fan_control_type": params.fan_control_type,
        "fan_max_temp": params.fan_max_temp,
        "fan_off_delta": params.fan_off_delta,
        "fan_start_temp": params.fan_start_temp,
        "fan_mode": params.fan_mode,
        "fan_monitoring_enabled": params.fan_monitoring_enabled,
        "fan2_monitoring_enabled": params.fan2_monitoring_enabled,
        "fan3_monitoring_enabled": params.fan3_monitoring_enabled,
        "fan4_monitoring_enabled": params.fan4_monitoring_enabled,
    }


def _load_params_db() -> dict:
    if not os.path.exists(PARAMS_DB):
        return {}
    try:
        with open(PARAMS_DB, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}
    return {}


def _save_params_db(raw: dict) -> None:
    with open(PARAMS_DB, "w", encoding="utf-8") as f:
        json.dump(raw, f, ensure_ascii=False, indent=2)


def load_device_params(address: str) -> DeviceParams:
    raw = _load_params_db()
    if isinstance(raw, dict):
        devices = raw.get("devices", {})
        if isinstance(devices, dict):
            device_raw = devices.get(address)
            if isinstance(device_raw, dict):
                parsed = _params_from_dict(device_raw)
                if parsed is not None:
                    return parsed
        parsed = _params_from_dict(raw.get("global", {}))
        if parsed is not None:
            return parsed
        parsed = _params_from_dict(raw)
        if parsed is not None:
            return parsed
    return DEFAULT_PARAMS


def save_device_params(address: str, params: DeviceParams) -> None:
    raw = _load_params_db()
    if not isinstance(raw, dict):
        raw = {}
    devices = raw.get("devices")
    if not isinstance(devices, dict):
        devices = {}
    devices[address] = _params_to_dict(params)
    raw["devices"] = devices
    _save_params_db(raw)


def load_params() -> DeviceParams:
    return load_device_params("_global")


def save_params(params: DeviceParams) -> None:
    raw = _load_params_db()
    if not isinstance(raw, dict):
        raw = {}
    raw["global"] = _params_to_dict(params)
    _save_params_db(raw)


def encode_params(params: DeviceParams, mask: int = 0x03FF) -> bytes:
    return struct.pack(
        "<BHiBiiiBBBBB",
        PARAMS_VERSION,
        mask & 0x03FF,
        params.fan_min_speed,
        params.fan_control_type,
        params.fan_max_temp,
        params.fan_off_delta,
        params.fan_start_temp,
        params.fan_mode,
        1 if params.fan_monitoring_enabled else 0,
        1 if params.fan2_monitoring_enabled else 0,
        1 if params.fan3_monitoring_enabled else 0,
        1 if params.fan4_monitoring_enabled else 0,
    )


def decode_params(data: bytes) -> DeviceParams:
    if len(data) != 25:
        raise RuntimeError(f"Bad params len={len(data)}")
    (
        version,
        mask,
        fan_min_speed,
        fan_control_type,
        fan_max_temp,
        fan_off_delta,
        fan_start_temp,
        fan_mode,
        fan_monitoring_enabled,
        fan2_monitoring_enabled,
        fan3_monitoring_enabled,
        fan4_monitoring_enabled,
    ) = struct.unpack(
        "<BHiBiiiBBBBB", data
    )
    if version != PARAMS_VERSION:
        raise RuntimeError(f"Unsupported params version={version}")
    _ = mask
    return DeviceParams(
        fan_min_speed=int(fan_min_speed),
        fan_control_type=int(fan_control_type),
        fan_max_temp=int(fan_max_temp),
        fan_off_delta=int(fan_off_delta),
        fan_start_temp=int(fan_start_temp),
        fan_mode=int(fan_mode),
        fan_monitoring_enabled=bool(fan_monitoring_enabled),
        fan2_monitoring_enabled=bool(fan2_monitoring_enabled),
        fan3_monitoring_enabled=bool(fan3_monitoring_enabled),
        fan4_monitoring_enabled=bool(fan4_monitoring_enabled),
    )


def decode_params_status(data: bytes) -> ParamsStatus:
    if len(data) != 3:
        raise RuntimeError(f"Bad params status len={len(data)}")
    version, status, field_id = struct.unpack("<BBB", data)
    if version != PARAMS_VERSION:
        raise RuntimeError(f"Unsupported params status version={version}")
    if field_id == PARAM_FIELD_NONE:
        field = None
    else:
        field = int(field_id)
    return ParamsStatus(ok=status == PARAM_STATUS_OK, status=status, field_id=field)


def decode_fan_status(data: bytes) -> FanStatus:
    if len(data) != 6:
        raise RuntimeError(f"Bad fan status len={len(data)}")
    version, s1, s2, s3, s4, op_type = struct.unpack("<BBBBBB", data)
    if version != FAN_STATUS_VERSION:
        raise RuntimeError(f"Unsupported fan status version={version}")
    states = (int(s1), int(s2), int(s3), int(s4))
    labels = tuple(FAN_STATE_NAMES.get(state, "UNKNOWN") for state in states)
    op_label = OP_TYPE_NAMES.get(op_type, "UNKNOWN")
    return FanStatus(states=states, labels=labels, op_type=int(op_type), op_label=op_label)


def decode_device_status(data: bytes) -> DeviceStatus:
    if len(data) != 3:
        raise RuntimeError(f"Bad device status len={len(data)}")
    version, state, error = struct.unpack("<BBB", data)
    if version != DEVICE_STATUS_VERSION:
        raise RuntimeError(f"Unsupported device status version={version}")
    return DeviceStatus(
        state=int(state),
        label=DEVICE_STATE_NAMES.get(state, "UNKNOWN"),
        error=int(error),
        error_label=DEVICE_ERROR_NAMES.get(error, "UNKNOWN"),
    )


def encode_operation_control(op_type: int, action: int = 1) -> bytes:
    return struct.pack("<BBB", OP_CONTROL_VERSION, op_type, action)


def decode_operation_status(data: bytes) -> OperationStatus:
    expected_len = 4 + OP_ERROR_TEXT_MAX
    if len(data) != expected_len:
        raise RuntimeError(f"Bad operation status len={len(data)}")
    version, op_type, state, err_len = struct.unpack("<BBBB", data[:4])
    if version != OP_STATUS_VERSION:
        raise RuntimeError(f"Unsupported operation status version={version}")
    err_len = min(int(err_len), OP_ERROR_TEXT_MAX)
    err_raw = data[4:4 + OP_ERROR_TEXT_MAX]
    err_text = ""
    if err_len > 0:
        err_text = err_raw[:err_len].decode("utf-8", errors="replace")
    return OperationStatus(op_type=int(op_type), state=int(state), error=err_text)


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

    def __init__(
        self,
        log: Optional[Callable[[str], None]] = None,
        adapter: Optional[str] = None,
        config: Optional[BleConfig] = None,
    ):
        self._log = log
        self._adapter = adapter
        self._config = config or DEFAULT_CONFIG
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

    async def scan_pairing(self, timeout: Optional[float] = None) -> list[DeviceInfo]:
        if timeout is None:
            timeout = self._config.scan_timeout_s
        candidates: dict[str, DeviceInfo] = {}

        def on_detect(device, advertisement_data) -> None:
            uuids = advertisement_data.service_uuids or []
            norm_uuids = {normalize_uuid(raw) for raw in uuids}
            name = advertisement_data.local_name or device.name or "Unknown"
            if PAIR_SVC_NORM in norm_uuids:
                candidates[device.address] = DeviceInfo(name=name, address=device.address)

        async with BleakScanner(detection_callback=on_detect, **self._scanner_kwargs()):
            await asyncio.sleep(timeout)

        results: dict[str, DeviceInfo] = {}
        for device in candidates.values():
            if await self._probe_pairing(device):
                results[device.address] = device

        return list(results.values())

    async def _probe_pairing(self, device: DeviceInfo) -> bool:
        client = BleakClient(device.address, adapter=self._adapter)
        try:
            await asyncio.wait_for(client.connect(), timeout=self._config.connect_timeout_s)
            await asyncio.wait_for(
                client.read_gatt_char(UUID_PAIR_DEV_NONCE),
                timeout=self._config.pair_timeout_s,
            )
            return True
        except Exception:
            return False
        finally:
            try:
                await client.disconnect()
            except Exception:
                pass

    async def resolve_device(self, address: str, timeout: Optional[float] = None) -> DeviceInfo:
        if timeout is None:
            timeout = self._config.resolve_timeout_s
        dev = await BleakScanner.find_device_by_address(
            address, timeout=timeout, **self._scanner_kwargs()
        )
        if not dev:
            raise RuntimeError(f"Device with address {address} was not found.")
        name = dev.name or "Unknown"
        return DeviceInfo(name=name, address=dev.address)

    async def connect_raw(self, device: DeviceInfo, connect_timeout: Optional[float] = None) -> None:
        if connect_timeout is None:
            connect_timeout = self._config.connect_timeout_s
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

    async def pair(self, device: DeviceInfo, timeout: Optional[float] = None) -> PairResult:
        if timeout is None:
            timeout = self._config.pair_timeout_s
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

    async def auth(self, timeout: Optional[float] = None) -> None:
        if timeout is None:
            timeout = self._config.auth_timeout_s
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

    async def read_metrics(
        self, timeout: Optional[float] = None, retries: Optional[int] = None
    ) -> list[float]:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        if retries is None:
            retries = self._config.metrics_retries
        last_exc: Optional[Exception] = None
        for attempt in range(retries + 1):
            try:
                return await self._read_metrics_once(timeout)
            except Exception as exc:
                last_exc = exc
                self._emit(f"Metrics read failed (attempt {attempt + 1}/{retries + 1}): {exc}")
                if attempt >= retries or not self.device:
                    break
                await self._reconnect_for_metrics(timeout)
        assert last_exc is not None
        raise last_exc

    async def _read_metrics_once(self, timeout: float) -> list[float]:
        if not self.client:
            raise RuntimeError("Not connected")
        values: list[float] = []
        for uuid in TEMP_CHAR_UUIDS:
            data = await asyncio.wait_for(self.client.read_gatt_char(uuid), timeout=timeout)
            value = decode_temp_value(bytes(data))
            if value is None:
                raise RuntimeError(f"Bad metrics value len={len(data)}")
            values.append(value)
        return values

    async def read_fan_speed(self, timeout: Optional[float] = None) -> float:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_FAN_SPEED_VALUE), timeout=timeout
        )
        value = decode_fan_speed(bytes(data))
        if value is None:
            raise RuntimeError(f"Bad fan speed value len={len(data)}")
        return value

    async def read_fan_speeds(self, timeout: Optional[float] = None) -> list[float]:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        values: list[float] = []
        for uuid in FAN_SPEED_UUIDS:
            data = await asyncio.wait_for(self.client.read_gatt_char(uuid), timeout=timeout)
            value = decode_fan_speed(bytes(data))
            if value is None:
                raise RuntimeError(f"Bad fan speed value len={len(data)}")
            values.append(value)
        return values

    async def read_params(self, timeout: Optional[float] = None) -> DeviceParams:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_CONFIG_PARAMS), timeout=timeout
        )
        return decode_params(bytes(data))

    async def read_fan_status(self, timeout: Optional[float] = None) -> FanStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_CONFIG_FAN_STATUS), timeout=timeout
        )
        return decode_fan_status(bytes(data))

    async def read_device_status(self, timeout: Optional[float] = None) -> DeviceStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_CONFIG_DEVICE_STATUS), timeout=timeout
        )
        return decode_device_status(bytes(data))

    async def read_operation_status(self, timeout: Optional[float] = None) -> OperationStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_OP_STATUS), timeout=timeout
        )
        return decode_operation_status(bytes(data))

    async def write_params(
        self, params: DeviceParams, timeout: Optional[float] = None, mask: int = 0x03FF
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        payload = encode_params(params, mask=mask)
        await asyncio.wait_for(
            self.client.write_gatt_char(UUID_CONFIG_PARAMS, payload, response=True),
            timeout=timeout,
        )

    async def apply_params(self, timeout: Optional[float] = None) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        await asyncio.wait_for(
            self.client.write_gatt_char(UUID_CONFIG_STATUS, b"\x01", response=True),
            timeout=timeout,
        )

    async def start_operation(self, op_type: int, timeout: Optional[float] = None) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        payload = encode_operation_control(op_type, action=1)
        await asyncio.wait_for(
            self.client.write_gatt_char(UUID_OP_CONTROL, payload, response=True),
            timeout=timeout,
        )

    async def start_fan_calibration(self, timeout: Optional[float] = None) -> None:
        await self.start_operation(OP_TYPE_FAN_CALIBRATION, timeout=timeout)

    async def start_setup_fans(self, timeout: Optional[float] = None) -> None:
        await self.start_operation(OP_TYPE_SETUP_FANS, timeout=timeout)

    async def _reconnect_for_metrics(self, timeout: float) -> None:
        try:
            await self.disconnect()
        except Exception:
            pass
        await asyncio.sleep(self._config.metrics_reconnect_delay_s)
        if not self.device:
            return
        await self.connect_raw(self.device, connect_timeout=max(self._config.connect_timeout_s, timeout))
        await self.auth(timeout=timeout)

    async def start_params_notify(self, callback: Callable[[ParamsStatus], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.start_notify(
            UUID_CONFIG_STATUS,
            lambda _, data: self._emit_params_status(callback, data),
        )

    async def stop_params_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self.client.stop_notify(UUID_CONFIG_STATUS)
        except Exception:
            pass

    async def start_fan_status_notify(self, callback: Callable[[FanStatus], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.start_notify(
            UUID_CONFIG_FAN_STATUS,
            lambda _, data: self._emit_fan_status(callback, data),
        )

    async def start_device_status_notify(
        self, callback: Callable[[DeviceStatus], None]
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.start_notify(
            UUID_CONFIG_DEVICE_STATUS,
            lambda _, data: self._emit_device_status(callback, data),
        )

    async def start_operation_status_notify(
        self, callback: Callable[[OperationStatus], None]
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.start_notify(
            UUID_OP_STATUS,
            lambda _, data: self._emit_operation_status(callback, data),
        )

    async def stop_fan_status_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self.client.stop_notify(UUID_CONFIG_FAN_STATUS)
        except Exception:
            pass

    async def stop_device_status_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self.client.stop_notify(UUID_CONFIG_DEVICE_STATUS)
        except Exception:
            pass

    async def stop_operation_status_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self.client.stop_notify(UUID_OP_STATUS)
        except Exception:
            pass

    async def start_metrics_notify(
        self,
        callback: Callable[[int, float], None],
        fan_callback: Optional[Callable[[int, float], None]] = None,
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        for idx, uuid in enumerate(TEMP_CHAR_UUIDS):
            await self.client.start_notify(
                uuid,
                lambda _, data, ch=idx: self._emit_temp(callback, ch, data),
            )
        if fan_callback is not None:
            for idx, uuid in enumerate(FAN_SPEED_UUIDS):
                try:
                    await self.client.start_notify(
                        uuid,
                        lambda _, data, ch=idx: self._emit_fan(fan_callback, ch, data),
                    )
                except Exception as exc:
                    self._emit(f"Fan {idx + 1} notify unavailable: {exc}")

    async def stop_metrics_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        for uuid in TEMP_CHAR_UUIDS:
            await self.client.stop_notify(uuid)
        for uuid in FAN_SPEED_UUIDS:
            try:
                await self.client.stop_notify(uuid)
            except Exception:
                pass

    @staticmethod
    def _emit_temp(callback: Callable[[int, float], None], channel: int, data: bytearray) -> None:
        value = decode_temp_value(bytes(data))
        if value is None:
            return
        callback(channel, value)

    @staticmethod
    def _emit_fan(callback: Callable[[int, float], None], channel: int, data: bytearray) -> None:
        value = decode_fan_speed(bytes(data))
        if value is None:
            return
        callback(channel, value)

    @staticmethod
    def _emit_params_status(callback: Callable[[ParamsStatus], None], data: bytearray) -> None:
        try:
            status = decode_params_status(bytes(data))
        except Exception:
            return
        callback(status)

    @staticmethod
    def _emit_fan_status(callback: Callable[[FanStatus], None], data: bytearray) -> None:
        try:
            status = decode_fan_status(bytes(data))
        except Exception:
            return
        callback(status)

    @staticmethod
    def _emit_device_status(callback: Callable[[DeviceStatus], None], data: bytearray) -> None:
        try:
            status = decode_device_status(bytes(data))
        except Exception:
            return
        callback(status)

    @staticmethod
    def _emit_operation_status(callback: Callable[[OperationStatus], None], data: bytearray) -> None:
        try:
            status = decode_operation_status(bytes(data))
        except Exception:
            return
        callback(status)
