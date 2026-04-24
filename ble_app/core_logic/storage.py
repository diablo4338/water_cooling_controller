from __future__ import annotations

import json
import os
import time
from typing import Optional

from platformdirs import user_config_dir

from .models import DEFAULT_PARAMS, DeviceInfo, DeviceParams
from .protocol import APP_NAME


def _ensure_app_config_dir() -> str:
    path = user_config_dir(APP_NAME, ensure_exists=True)
    os.makedirs(path, exist_ok=True)
    return path


APP_CONFIG_DIR = _ensure_app_config_dir()
PAIRED_DB = os.path.join(APP_CONFIG_DIR, "paired_devices.json")
HOST_KEY_PATH = os.path.join(APP_CONFIG_DIR, "host_key.pem")
PARAMS_DB = os.path.join(APP_CONFIG_DIR, "params.json")


def load_paired_records() -> list[dict]:
    if not os.path.exists(PAIRED_DB):
        return []
    try:
        with open(PAIRED_DB, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
        if isinstance(raw, list):
            return raw
    except Exception:
        return []
    return []


def save_paired_records(records: list[dict]) -> None:
    with open(PAIRED_DB, "w", encoding="utf-8") as handle:
        json.dump(records, handle, ensure_ascii=False, indent=2)


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


def add_or_update_paired(device: DeviceInfo, k_hex: str) -> None:
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
    except Exception:
        return None


def _params_to_dict(params: DeviceParams) -> dict:
    return {
        "fan_min_speed": int(params.fan_min_speed),
        "fan_control_type": int(params.fan_control_type),
        "fan_max_temp": int(params.fan_max_temp),
        "fan_off_delta": int(params.fan_off_delta),
        "fan_start_temp": int(params.fan_start_temp),
        "fan_mode": int(params.fan_mode),
        "fan_monitoring_enabled": bool(params.fan_monitoring_enabled),
        "fan2_monitoring_enabled": bool(params.fan2_monitoring_enabled),
        "fan3_monitoring_enabled": bool(params.fan3_monitoring_enabled),
        "fan4_monitoring_enabled": bool(params.fan4_monitoring_enabled),
    }


def _load_params_db() -> dict:
    if not os.path.exists(PARAMS_DB):
        return {}
    try:
        with open(PARAMS_DB, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
        if isinstance(raw, dict):
            return raw
    except Exception:
        return {}
    return {}


def _save_params_db(raw: dict) -> None:
    with open(PARAMS_DB, "w", encoding="utf-8") as handle:
        json.dump(raw, handle, ensure_ascii=False, indent=2)


def load_device_params(address: str) -> DeviceParams:
    raw = _load_params_db()
    item = raw.get(address)
    if isinstance(item, dict):
        params = _params_from_dict(item)
        if params is not None:
            return params
    return DEFAULT_PARAMS


def save_device_params(address: str, params: DeviceParams) -> None:
    raw = _load_params_db()
    raw[address] = _params_to_dict(params)
    _save_params_db(raw)


def load_params() -> DeviceParams:
    return load_device_params("__global__")


def save_params(params: DeviceParams) -> None:
    save_device_params("__global__", params)
