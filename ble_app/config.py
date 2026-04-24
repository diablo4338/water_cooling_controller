from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Optional


def _get_float(name: str, default: float) -> float:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    try:
        return float(raw)
    except ValueError:
        return default


def _get_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError:
        return default


def _get_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _get_optional_bool(name: str) -> Optional[bool]:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return None
    value = raw.strip().lower()
    if value in {"1", "true", "yes", "on"}:
        return True
    if value in {"0", "false", "no", "off"}:
        return False
    return None


def _get_optional_str(name: str) -> Optional[str]:
    raw = os.getenv(name)
    if raw is None:
        return None
    value = raw.strip()
    return value or None


@dataclass(frozen=True)
class BleConfig:
    scan_timeout_s: float
    resolve_timeout_s: float
    connect_timeout_s: float
    pair_timeout_s: float
    auth_timeout_s: float
    metrics_timeout_s: float
    metrics_retries: int
    metrics_reconnect_delay_s: float
    gui_action_default_timeout_s: float
    gui_action_scan_timeout_s: float
    gui_action_pair_timeout_s: float
    gui_action_connect_timeout_s: float
    gui_action_disconnect_timeout_s: float
    resolve_before_connect: bool
    use_service_filter: bool
    winrt_use_cached_services: Optional[bool]
    winrt_address_type: Optional[str]


def load_config() -> BleConfig:
    scan_timeout_s = _get_float("BLE_SCAN_TIMEOUT_S", 5.0)
    resolve_timeout_s = _get_float("BLE_RESOLVE_TIMEOUT_S", 5.0)
    connect_timeout_s = _get_float("BLE_CONNECT_TIMEOUT_S", 5.0)
    pair_timeout_s = _get_float("BLE_PAIR_TIMEOUT_S", 8.0)
    auth_timeout_s = _get_float("BLE_AUTH_TIMEOUT_S", 5.0)
    metrics_timeout_s = _get_float("BLE_METRICS_TIMEOUT_S", 5.0)
    metrics_retries = _get_int("BLE_METRICS_RETRIES", 1)
    metrics_reconnect_delay_s = _get_float("BLE_METRICS_RECONNECT_DELAY_S", 0.3)
    gui_action_default_timeout_s = _get_float("GUI_ACTION_DEFAULT_TIMEOUT_S", 5.0)
    gui_action_scan_timeout_s = _get_float("GUI_ACTION_SCAN_TIMEOUT_S", 10.0)
    gui_action_pair_timeout_s = _get_float("GUI_ACTION_PAIR_TIMEOUT_S", pair_timeout_s)
    gui_action_connect_timeout_s = _get_float("GUI_ACTION_CONNECT_TIMEOUT_S", connect_timeout_s)
    gui_action_disconnect_timeout_s = _get_float("GUI_ACTION_DISCONNECT_TIMEOUT_S", 5.0)
    resolve_before_connect = _get_bool("BLE_RESOLVE_BEFORE_CONNECT", os.name == "nt")
    use_service_filter = _get_bool("BLE_USE_SERVICE_FILTER", True)
    winrt_use_cached_services = _get_optional_bool("BLE_WINRT_USE_CACHED_SERVICES")
    winrt_address_type = _get_optional_str("BLE_WINRT_ADDRESS_TYPE")

    return BleConfig(
        scan_timeout_s=scan_timeout_s,
        resolve_timeout_s=resolve_timeout_s,
        connect_timeout_s=connect_timeout_s,
        pair_timeout_s=pair_timeout_s,
        auth_timeout_s=auth_timeout_s,
        metrics_timeout_s=metrics_timeout_s,
        metrics_retries=metrics_retries,
        metrics_reconnect_delay_s=metrics_reconnect_delay_s,
        gui_action_default_timeout_s=gui_action_default_timeout_s,
        gui_action_scan_timeout_s=gui_action_scan_timeout_s,
        gui_action_pair_timeout_s=gui_action_pair_timeout_s,
        gui_action_connect_timeout_s=gui_action_connect_timeout_s,
        gui_action_disconnect_timeout_s=gui_action_disconnect_timeout_s,
        resolve_before_connect=resolve_before_connect,
        use_service_filter=use_service_filter,
        winrt_use_cached_services=winrt_use_cached_services,
        winrt_address_type=winrt_address_type,
    )


DEFAULT_CONFIG = load_config()
