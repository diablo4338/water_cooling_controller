from __future__ import annotations

from ble_app.config import load_config


def test_load_config_parses_windows_ble_overrides(monkeypatch) -> None:
    monkeypatch.setenv("BLE_RESOLVE_BEFORE_CONNECT", "1")
    monkeypatch.setenv("BLE_USE_SERVICE_FILTER", "0")
    monkeypatch.setenv("BLE_WINRT_USE_CACHED_SERVICES", "true")
    monkeypatch.setenv("BLE_WINRT_ADDRESS_TYPE", "random")

    config = load_config()

    assert config.resolve_before_connect is True
    assert config.use_service_filter is False
    assert config.winrt_use_cached_services is True
    assert config.winrt_address_type == "random"


def test_load_config_ignores_invalid_optional_bool(monkeypatch) -> None:
    monkeypatch.setenv("BLE_WINRT_USE_CACHED_SERVICES", "maybe")

    config = load_config()

    assert config.winrt_use_cached_services is None
