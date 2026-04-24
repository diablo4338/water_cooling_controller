from __future__ import annotations

import asyncio
from typing import Callable, Optional

from bleak import BleakClient

from .codec import (
    decode_device_status,
    decode_fan_status,
    decode_operation_status,
    decode_params,
    decode_params_status,
    encode_operation_control,
    encode_params,
)
from .models import DeviceInfo, DeviceStatus, FanStatus, OperationStatus, ParamsStatus
from .protocol import (
    OP_TYPE_FAN_CALIBRATION,
    OP_TYPE_SETUP_FANS,
    UUID_CONFIG_DEVICE_STATUS,
    UUID_CONFIG_FAN_STATUS,
    UUID_CONFIG_PARAMS,
    UUID_CONFIG_STATUS,
    UUID_OP_CONTROL,
    UUID_OP_STATUS,
)


class BleCoreControlMixin:
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

    async def read_params(self, timeout: Optional[float] = None):
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(self.client.read_gatt_char(UUID_CONFIG_PARAMS), timeout=timeout)
        return decode_params(bytes(data))

    async def read_fan_status(self, timeout: Optional[float] = None) -> FanStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_CONFIG_FAN_STATUS),
            timeout=timeout,
        )
        return decode_fan_status(bytes(data))

    async def read_device_status(self, timeout: Optional[float] = None) -> DeviceStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(
            self.client.read_gatt_char(UUID_CONFIG_DEVICE_STATUS),
            timeout=timeout,
        )
        return decode_device_status(bytes(data))

    async def read_operation_status(self, timeout: Optional[float] = None) -> OperationStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await asyncio.wait_for(self.client.read_gatt_char(UUID_OP_STATUS), timeout=timeout)
        return decode_operation_status(bytes(data))

    async def write_params(self, params, timeout: Optional[float] = None, mask: int = 0x03FF) -> None:
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

    async def start_device_status_notify(self, callback: Callable[[DeviceStatus], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self.client.start_notify(
            UUID_CONFIG_DEVICE_STATUS,
            lambda _, data: self._emit_device_status(callback, data),
        )

    async def start_operation_status_notify(
        self,
        callback: Callable[[OperationStatus], None],
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
    def _emit_operation_status(
        callback: Callable[[OperationStatus], None],
        data: bytearray,
    ) -> None:
        try:
            status = decode_operation_status(bytes(data))
        except Exception:
            return
        callback(status)
