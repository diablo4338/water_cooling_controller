from __future__ import annotations

from typing import Callable, Optional

from bleak import BleakClient

from ..config import BleConfig, DEFAULT_CONFIG
from .crypto import load_or_create_host_key
from .models import DeviceInfo


class BleCoreBaseMixin:
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

    def _scanner_kwargs(self) -> dict:
        if self._adapter:
            return {"bluez": {"adapter": self._adapter}}
        return {}
