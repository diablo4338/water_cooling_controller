from __future__ import annotations

import asyncio
import sys
import time
from typing import Any, Awaitable, Callable, Optional

from bleak import BleakClient
from bleak import BleakScanner

from ..config import BleConfig, DEFAULT_CONFIG
from .crypto import load_or_create_host_key
from .models import DeviceInfo
from .protocol import UUID_LABELS


class BleCoreBaseMixin:
    CONNECTION_LOG_PREFIX = "[BLE-CONN]"

    def __init__(
        self,
        log: Optional[Callable[[str], None]] = None,
        adapter: Optional[str] = None,
        config: Optional[BleConfig] = None,
    ) -> None:
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

    def _emit_conn(self, msg: str) -> None:
        self._emit(f"{self.CONNECTION_LOG_PREFIX} {msg}")

    @staticmethod
    def _format_error(exc: BaseException) -> str:
        text = str(exc).strip()
        if text:
            return f"{type(exc).__name__}: {text}"
        return f"{type(exc).__name__}: {exc!r}"

    @staticmethod
    def _format_duration(started_at: float) -> str:
        return f"{(time.monotonic() - started_at) * 1000:.0f} ms"

    @staticmethod
    def _format_timeout(timeout: Optional[float]) -> str:
        if timeout is None:
            return "none"
        return f"{timeout:.1f}s"

    @staticmethod
    def _format_device(device: DeviceInfo) -> str:
        return f"{device.name} ({device.address})"

    @staticmethod
    def _bool_state(value: bool) -> str:
        return "yes" if value else "no"

    @staticmethod
    def _uuid_label(uuid: str) -> str:
        return UUID_LABELS.get(uuid, uuid)

    async def _run_step(
        self,
        label: str,
        awaitable: Awaitable[Any],
        *,
        timeout: Optional[float] = None,
    ) -> Any:
        started_at = time.monotonic()
        self._emit_conn(f"{label}: start (timeout={self._format_timeout(timeout)})")
        try:
            if timeout is None:
                result = await awaitable
            else:
                result = await asyncio.wait_for(awaitable, timeout=timeout)
        except Exception as exc:
            self._emit_conn(
                f"{label}: failed after {self._format_duration(started_at)}: {self._format_error(exc)}"
            )
            raise
        self._emit_conn(f"{label}: ok in {self._format_duration(started_at)}")
        return result

    def _make_client(
        self,
        target: Any,
        *,
        connect_timeout: float,
        services: Optional[list[str]] = None,
    ) -> BleakClient:
        kwargs: dict[str, Any] = {"timeout": connect_timeout}
        if self._adapter:
            kwargs["adapter"] = self._adapter
        if services and self._config.use_service_filter:
            kwargs["services"] = services
        if sys.platform == "win32":
            winrt: dict[str, Any] = {}
            if self._config.winrt_use_cached_services is not None:
                winrt["use_cached_services"] = self._config.winrt_use_cached_services
            if self._config.winrt_address_type:
                winrt["address_type"] = self._config.winrt_address_type
            if winrt:
                kwargs["winrt"] = winrt
        return BleakClient(target, **kwargs)

    async def _resolve_ble_target(
        self,
        device: DeviceInfo,
        *,
        timeout: Optional[float] = None,
    ) -> Any:
        if device.ble_device is not None:
            self._emit_conn(f"resolve {self._format_device(device)}: reusing cached BLEDevice")
            return device.ble_device
        if not self._config.resolve_before_connect:
            self._emit_conn(
                f"resolve {self._format_device(device)}: skipped (using address directly)"
            )
            return device.address
        ble_device = await self._run_step(
            f"resolve {self._format_device(device)}",
            BleakScanner.find_device_by_address(
                device.address,
                timeout=timeout or self._config.resolve_timeout_s,
                **self._scanner_kwargs(),
            ),
            timeout=(timeout or self._config.resolve_timeout_s) + 0.5,
        )
        if ble_device is None:
            raise RuntimeError(f"Device with address {device.address} was not found.")
        resolved_name = getattr(ble_device, "name", None) or device.name
        self._emit_conn(
            "resolved target: "
            f"name={resolved_name}, address={getattr(ble_device, 'address', device.address)}"
        )
        return ble_device

    async def _read_char(self, uuid: str, *, timeout: float, label: Optional[str] = None) -> bytearray:
        if not self.client:
            raise RuntimeError("Not connected")
        step_label = label or f"read {self._uuid_label(uuid)}"
        data = await self._run_step(step_label, self.client.read_gatt_char(uuid), timeout=timeout)
        self._emit_conn(f"{step_label}: received {len(data)} bytes")
        return data

    async def _write_char(
        self,
        uuid: str,
        data: bytes,
        *,
        response: bool,
        timeout: float,
        label: Optional[str] = None,
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        step_label = label or f"write {self._uuid_label(uuid)}"
        self._emit_conn(
            f"{step_label}: sending {len(data)} bytes (response={self._bool_state(response)})"
        )
        await self._run_step(
            step_label,
            self.client.write_gatt_char(uuid, data, response=response),
            timeout=timeout,
        )

    async def _start_notify(
        self,
        uuid: str,
        callback: Callable[[Any, bytearray], None],
        *,
        label: Optional[str] = None,
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        step_label = label or f"start notify {self._uuid_label(uuid)}"
        await self._run_step(step_label, self.client.start_notify(uuid, callback), timeout=3.0)

    async def _stop_notify(self, uuid: str, *, label: Optional[str] = None) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        step_label = label or f"stop notify {self._uuid_label(uuid)}"
        await self._run_step(step_label, self.client.stop_notify(uuid), timeout=3.0)

    def _scanner_kwargs(self) -> dict:
        if self._adapter:
            return {"bluez": {"adapter": self._adapter}}
        return {}
