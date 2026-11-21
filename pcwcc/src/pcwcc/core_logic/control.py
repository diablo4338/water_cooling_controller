from __future__ import annotations

import asyncio
from contextlib import suppress
from typing import Callable, Optional, TYPE_CHECKING

from bleak import BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.exc import BleakCharacteristicNotFoundError

from .codec import (
    decode_device_status,
    decode_fan_status,
    decode_operation_status,
    decode_params,
    decode_params_status,
    decode_profile_metadata,
    decode_profile_slots,
    encode_operation_control,
    encode_profile_metadata,
    encode_profile_command,
    encode_params,
)
from .models import (
    DeviceInfo, DeviceParams, DeviceStatus, FanModeStatus, FanStatus, OperationStatus, OutputLimitResetResult,
    OutputLimitState, OutputLimitStatus, ParamsStatus, ProfileApplyResult, ProfileSkipReason,
    ThermalInputState,
)
from .protocol import (
    MAIN_SERVICE_UUIDS,
    OP_TYPE_FAN_CALIBRATION,
    OP_TYPE_NONE,
    OP_TYPE_SETUP_FANS,
    PARAM_STATUS_INVALID,
    PARAM_STATUS_OUTPUT_ADJUSTING,
    PARAM_STATUS_STORAGE_ERROR,
    PROFILE_ACTION_APPLY,
    PROFILE_ACTION_CLEAR,
    PROFILE_ACTION_SAVE_CURRENT,
    UUID_CONFIG_DEVICE_STATUS,
    UUID_CONFIG_FAN_STATUS,
    UUID_CONFIG_PARAMS,
    UUID_CONFIG_OUTPUT_LIMIT,
    UUID_CONFIG_OUTPUT_RANGE,
    UUID_CONFIG_FAN_MODE_STATUS,
    UUID_CONFIG_PROFILE_RESULT,
    UUID_CONFIG_PROFILE_METADATA,
    UUID_CONFIG_PROFILES,
    UUID_CONFIG_STATUS,
    UUID_OP_CONTROL,
    UUID_OP_STATUS,
)


if TYPE_CHECKING:
    from .api import BleAppCore


class BleCoreControlMixin:
    async def connect_raw(
        self: BleAppCore, device: DeviceInfo, connect_timeout: Optional[float] = None, *,
        disconnected_callback: Optional[Callable[[BleakClient], None]] = None,
    ) -> None:
        if connect_timeout is None:
            connect_timeout = self._config.connect_timeout_s
        self._emit_conn(
            f"connect {self._format_device(device)}: start (timeout={connect_timeout:.1f}s)"
        )
        await self.disconnect()
        target = await self._resolve_ble_target(device, timeout=self._config.resolve_timeout_s)
        client = self._make_client(
            target,
            connect_timeout=connect_timeout,
            services=MAIN_SERVICE_UUIDS,
            disconnected_callback=disconnected_callback,
        )
        try:
            await self._run_step(
                f"connect {self._format_device(device)}",
                client.connect(),
                timeout=connect_timeout,
            )
        except (Exception, asyncio.CancelledError):
            with suppress(Exception):
                await self._run_step("disconnect after failed connect", client.disconnect(), timeout=3.0)
            raise
        self.client = client
        self.device = device if isinstance(target, str) else device.with_ble_device(target)
        backend_id = getattr(client, "backend_id", None)
        self._emit_conn(
            "connect established: "
            f"backend={backend_id or 'unknown'}, is_connected={self._bool_state(client.is_connected)}"
        )

    async def disconnect(self: BleAppCore) -> None:
        try:
            if self.client:
                try:
                    label = (
                        f"disconnect {self._format_device(self.device)}"
                        if self.device
                        else "disconnect current client"
                    )
                    await self._run_step(label, self.client.disconnect(), timeout=3.0)
                except Exception as exc:
                    self._emit_conn(f"disconnect cleanup error: {self._format_error(exc)}")
        finally:
            self.client = None
            self.device = None
            self._output_limit_notify_uuid = None
            self._last_output_limit = None
            if self._output_reset_waiter is not None:
                self._output_reset_waiter[1].cancel()
                self._output_reset_waiter = None

    async def read_params(self: BleAppCore, timeout: Optional[float] = None) -> DeviceParams:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await self._read_char(UUID_CONFIG_PARAMS, timeout=timeout)
        return decode_params(bytes(data))

    async def read_profile_slots(self: BleAppCore, timeout: Optional[float] = None) -> frozenset[int]:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await self._read_char(UUID_CONFIG_PROFILES, timeout=timeout)
        return decode_profile_slots(bytes(data))

    async def read_profile_metadata(
        self: BleAppCore, slot: int, timeout: Optional[float] = None
    ) -> dict[str, str]:
        if not self.client:
            raise RuntimeError("Not connected")
        if not 1 <= slot <= len(UUID_CONFIG_PROFILE_METADATA):
            raise ValueError("Invalid profile slot")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await self._read_char(UUID_CONFIG_PROFILE_METADATA[slot - 1], timeout=timeout)
        return decode_profile_metadata(bytes(data))

    async def save_profile_metadata(
        self: BleAppCore, slot: int, title: str, description: str, timeout: Optional[float] = None
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if not 1 <= slot <= len(UUID_CONFIG_PROFILE_METADATA):
            raise ValueError("Invalid profile slot")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        await self._write_char(
            UUID_CONFIG_PROFILE_METADATA[slot - 1],
            encode_profile_metadata(title, description),
            response=True,
            timeout=timeout,
        )

    async def _profile_command(
        self: BleAppCore, action: int, slot: int, timeout: Optional[float] = None
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        await self._write_char(
            UUID_CONFIG_PROFILES,
            encode_profile_command(action, slot),
            response=True,
            timeout=timeout,
        )

    async def save_profile(self: BleAppCore, slot: int, timeout: Optional[float] = None) -> None:
        await self._profile_command(PROFILE_ACTION_SAVE_CURRENT, slot, timeout)

    async def apply_profile(self: BleAppCore, slot: int, timeout: Optional[float] = None) -> None:
        await self._profile_command(PROFILE_ACTION_APPLY, slot, timeout)

    async def clear_profile(self: BleAppCore, slot: int, timeout: Optional[float] = None) -> None:
        await self._profile_command(PROFILE_ACTION_CLEAR, slot, timeout)

    async def read_fan_mode_status(self: BleAppCore, *, timeout: float) -> FanModeStatus:
        data = await self._read_char(UUID_CONFIG_FAN_MODE_STATUS, timeout=timeout)
        return self._decode_fan_mode_status(data)

    @staticmethod
    def _decode_fan_mode_status(data: bytes | bytearray) -> FanModeStatus:
        if len(data) != 4 or data[0] != 1 or data[1] not in range(3) or data[2] not in range(4):
            raise RuntimeError("Invalid fan mode status")
        return FanModeStatus(data[1], data[2], ThermalInputState(data[3]))

    async def read_profile_apply_result(self: BleAppCore, slot: int, *, timeout: float) -> ProfileApplyResult:
        data = await self._read_char(UUID_CONFIG_PROFILE_RESULT, timeout=timeout)
        if len(data) != 16 or data[0] != 1 or data[1] != slot:
            raise RuntimeError("Controller did not return the result for the requested profile")
        return ProfileApplyResult(
            slot=slot, status=data[2], applied_mask=int.from_bytes(data[3:5], "little"),
            skipped=tuple(ProfileSkipReason(reason) for reason in data[5:]),
        )

    async def read_output_limit(self: BleAppCore, *, timeout: float) -> OutputLimitStatus:
        previous = self._last_output_limit
        try:
            data = await self._read_char(self._output_limit_notify_uuid or UUID_CONFIG_OUTPUT_RANGE, timeout=timeout)
        except BleakCharacteristicNotFoundError:
            data = await self._read_char(UUID_CONFIG_OUTPUT_LIMIT, timeout=timeout)
        status = self._decode_output_limit(data)
        if self._last_output_limit is not None and self._last_output_limit is not previous:
            return self._last_output_limit
        self._last_output_limit = status
        return status

    @staticmethod
    def _decode_output_limit(data: bytes | bytearray) -> OutputLimitStatus:
        if not data or (data[0], len(data)) not in ((1, 4), (2, 6), (3, 8)):
            raise RuntimeError("Invalid output-limit status")
        if data[1] > 100 or data[2] > data[1]:
            raise RuntimeError("Invalid output-limit status")
        minimum = data[6] if data[0] == 3 and data[6] != 255 else None
        if data[0] == 3 and (data[7] > 1 or (minimum is not None and minimum > data[2])):
            raise RuntimeError("Invalid output-limit status")
        return OutputLimitStatus(
            configured=data[1], effective=data[2], state=OutputLimitState(data[3]),
            reset_sequence=data[4] if data[0] >= 2 else None,
            reset_result=OutputLimitResetResult(data[5]) if data[0] >= 2 else OutputLimitResetResult.NONE,
            temporary_minimum=minimum,
            alert_without_load=data[0] == 3 and bool(data[7]),
        )

    async def set_output_max(self: BleAppCore, maximum: int, *, timeout: float) -> None:
        if not 1 <= maximum <= 100:
            raise ValueError("Maximum output must be between 1% and 100%")
        try:
            await self._write_char(UUID_CONFIG_OUTPUT_LIMIT, bytes((1, 1, maximum)), response=True, timeout=timeout)
        except Exception as exc:
            status = None
            with suppress(Exception):
                status = await self.read_params_status(timeout=timeout)
            if status is not None:
                message = {
                    PARAM_STATUS_INVALID: "Maximum output was rejected by the controller.",
                    PARAM_STATUS_OUTPUT_ADJUSTING: "Maximum was not saved. Wait for overload protection to finish.",
                    PARAM_STATUS_STORAGE_ERROR: "Could not save the maximum output to device storage.",
                }.get(status.status)
                if message:
                    raise RuntimeError(message) from exc
            raise

    async def reset_output_limit(self: BleAppCore, *, timeout: float) -> OutputLimitStatus:
        previous = self._last_output_limit
        if self._output_limit_notify_uuid is None or previous is None or previous.reset_sequence is None:
            raise RuntimeError("Firmware with output-limit notifications is required to confirm the reset")
        if self._output_reset_waiter is not None:
            raise RuntimeError("An overload reset is already in progress")

        future = asyncio.get_running_loop().create_future()
        sequence = (previous.reset_sequence + 1) & 0xFF
        waiter = (sequence, future)
        self._output_reset_waiter = waiter

        async def reset_and_confirm() -> OutputLimitStatus:
            await self._write_char(UUID_CONFIG_OUTPUT_LIMIT, bytes((1, 2, 0)), response=True, timeout=timeout)
            status = await future
            if status.reset_result == OutputLimitResetResult.REJECTED:
                raise RuntimeError("Temporary limit was not reset: overload active or sensor unavailable")
            return status

        try:
            return await asyncio.wait_for(reset_and_confirm(), timeout=timeout)
        except asyncio.TimeoutError as exc:
            raise RuntimeError("Controller did not confirm the temporary limit reset") from exc
        finally:
            if self._output_reset_waiter is waiter:
                self._output_reset_waiter = None
            future.cancel()

    def _supports_notify(self: BleAppCore, uuid: str) -> bool:
        if self.client is None:
            raise RuntimeError("Not connected")
        characteristic = self.client.services.get_characteristic(uuid)
        return characteristic is not None and "notify" in characteristic.properties

    @property
    def output_limit_notifications_active(self: BleAppCore) -> bool:
        return self._output_limit_notify_uuid is not None

    async def start_fan_mode_status_notify(self: BleAppCore, callback: Callable[[FanModeStatus], None]) -> None:
        if not self._supports_notify(UUID_CONFIG_FAN_MODE_STATUS):
            return
        client = self.client

        def on_notify(_: BleakGATTCharacteristic, data: bytearray) -> None:
            if self.client is not client:
                return
            try:
                status = self._decode_fan_mode_status(data)
            except (RuntimeError, ValueError):
                return
            callback(status)

        await self._start_notify(UUID_CONFIG_FAN_MODE_STATUS, on_notify)

    async def stop_fan_mode_status_notify(self: BleAppCore) -> None:
        if self._supports_notify(UUID_CONFIG_FAN_MODE_STATUS):
            await self._stop_notify(UUID_CONFIG_FAN_MODE_STATUS)

    async def start_output_limit_notify(self: BleAppCore, callback: Callable[[OutputLimitStatus], None]) -> None:
        uuid = next((uuid for uuid in (UUID_CONFIG_OUTPUT_RANGE, UUID_CONFIG_OUTPUT_LIMIT)
                     if self._supports_notify(uuid)), None)
        if uuid is None:
            return
        client = self.client
        self._output_limit_notify_uuid = uuid

        def on_notify(_: BleakGATTCharacteristic, data: bytearray) -> None:
            if self.client is not client or self._output_limit_notify_uuid != uuid:
                return
            try:
                status = self._decode_output_limit(data)
            except (RuntimeError, ValueError):
                return
            self._last_output_limit = status
            waiter = self._output_reset_waiter
            if waiter is not None:
                sequence, future = waiter
                if (status.reset_sequence == sequence and not future.done() and
                        status.reset_result in (OutputLimitResetResult.APPLIED, OutputLimitResetResult.REJECTED)):
                    future.set_result(status)
            callback(status)

        try:
            await self._start_notify(uuid, on_notify)
        except (Exception, asyncio.CancelledError):
            self._output_limit_notify_uuid = None
            raise

    async def stop_output_limit_notify(self: BleAppCore) -> None:
        uuid = self._output_limit_notify_uuid
        self._output_limit_notify_uuid = None
        if uuid is not None:
            await self._stop_notify(uuid)

    async def read_fan_status(self: BleAppCore, timeout: Optional[float] = None) -> FanStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await self._read_char(UUID_CONFIG_FAN_STATUS, timeout=timeout)
        return decode_fan_status(bytes(data))

    async def read_device_status(self: BleAppCore, timeout: Optional[float] = None) -> DeviceStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await self._read_char(UUID_CONFIG_DEVICE_STATUS, timeout=timeout)
        return decode_device_status(bytes(data))

    async def read_operation_status(self: BleAppCore, timeout: Optional[float] = None) -> OperationStatus:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        data = await self._read_char(UUID_OP_STATUS, timeout=timeout)
        return decode_operation_status(bytes(data))

    async def write_params(self: BleAppCore, params: DeviceParams, timeout: Optional[float] = None, mask: int = 0x07FF) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        payload = encode_params(params, mask=mask)
        await self._write_char(
            UUID_CONFIG_PARAMS,
            payload,
            response=True,
            timeout=timeout,
        )

    async def read_params_status(self: BleAppCore, timeout: Optional[float] = None) -> ParamsStatus:
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        return decode_params_status(bytes(await self._read_char(UUID_CONFIG_STATUS, timeout=timeout)))

    async def apply_params(self: BleAppCore, timeout: Optional[float] = None) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        await self._write_char(
            UUID_CONFIG_STATUS,
            b"\x01",
            response=True,
            timeout=timeout,
        )

    async def start_operation(self: BleAppCore, op_type: int, timeout: Optional[float] = None) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        payload = encode_operation_control(op_type, action=1)
        await self._write_char(
            UUID_OP_CONTROL,
            payload,
            response=True,
            timeout=timeout,
        )

    async def cancel_operation(self: BleAppCore, *, timeout: float) -> None:
        await self._write_char(
            UUID_OP_CONTROL,
            encode_operation_control(OP_TYPE_NONE, action=2),
            response=True,
            timeout=timeout,
        )

    async def start_fan_calibration(self: BleAppCore, timeout: Optional[float] = None) -> None:
        await self.start_operation(OP_TYPE_FAN_CALIBRATION, timeout=timeout)

    async def start_setup_fans(self: BleAppCore, timeout: Optional[float] = None) -> None:
        await self.start_operation(OP_TYPE_SETUP_FANS, timeout=timeout)

    async def start_params_notify(self: BleAppCore, callback: Callable[[ParamsStatus], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self._start_notify(
            UUID_CONFIG_STATUS,
            lambda _, data: self._emit_params_status(callback, data),
        )

    async def stop_params_notify(self: BleAppCore) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self._stop_notify(UUID_CONFIG_STATUS)
        except Exception:
            pass

    async def start_fan_status_notify(self: BleAppCore, callback: Callable[[FanStatus], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self._start_notify(
            UUID_CONFIG_FAN_STATUS,
            lambda _, data: self._emit_fan_status(callback, data),
        )

    async def start_device_status_notify(self: BleAppCore, callback: Callable[[DeviceStatus], None]) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self._start_notify(
            UUID_CONFIG_DEVICE_STATUS,
            lambda _, data: self._emit_device_status(callback, data),
        )

    async def start_operation_status_notify(
        self: BleAppCore,
        callback: Callable[[OperationStatus], None],
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        await self._start_notify(
            UUID_OP_STATUS,
            lambda _, data: self._emit_operation_status(callback, data),
        )

    async def stop_fan_status_notify(self: BleAppCore) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self._stop_notify(UUID_CONFIG_FAN_STATUS)
        except Exception:
            pass

    async def stop_device_status_notify(self: BleAppCore) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self._stop_notify(UUID_CONFIG_DEVICE_STATUS)
        except Exception:
            pass

    async def stop_operation_status_notify(self: BleAppCore) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        try:
            await self._stop_notify(UUID_OP_STATUS)
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
