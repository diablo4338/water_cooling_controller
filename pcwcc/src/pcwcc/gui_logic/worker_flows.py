from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
from contextlib import suppress

from typing import Optional, TYPE_CHECKING, TypeVar

from bleak.exc import BleakCharacteristicNotFoundError
from PySide6.QtCore import SignalInstance

from ..core import (
    DeviceInfo,
    DeviceParams,
    FanStatus,
    MetricsSnapshot,
    OP_STATE_IN_SERVICE,
    OP_TYPE_FAN_CALIBRATION,
    OP_TYPE_SETUP_FANS,
    OperationStatus,
    PROFILE_ACTION_CLEAR,
    PROFILE_ACTION_SAVE_CURRENT,
    PROFILE_COUNT,
    load_paired_records,
)

from ..presentation import ConnEvent, ConnEventKind


if TYPE_CHECKING:
    from .app import BleWorker


_Status = TypeVar("_Status")


class BleWorkerFlowMixin:
    @staticmethod
    def _conn_message(message: str) -> str:
        return f"[BLE-CONN] {message}"

    @staticmethod
    def _format_error(exc: BaseException | None) -> str:
        if exc is None:
            return "unknown"
        text = str(exc).strip()
        if text:
            return text
        return f"{type(exc).__name__}: {exc!r}"

    async def scan(self: BleWorker) -> None:
        self.log.emit("Scanning for devices in pairing mode...")
        try:
            devices = await self.core.scan_pairing(timeout=self.config.scan_timeout_s)
        except Exception as exc:
            self.log.emit(f"Scan error: {exc}")
            self.scan_results.emit([])
            return
        self.scan_results.emit(devices)
        self.log.emit(f"Pairing-ready devices found: {len(devices)}")

    async def auto_connect_saved(self: BleWorker, stop_event: asyncio.Event) -> None:
        stop = asyncio.create_task(stop_event.wait())
        try:
            while not stop_event.is_set():
                saved = self._load_saved_devices()
                if not saved:
                    self.log.emit("No saved devices.")
                    await asyncio.wait({stop}, timeout=2.0)
                    continue
                for device in saved:
                    if stop_event.is_set():
                        break
                    connection_id = object()
                    self.connection_event.emit(ConnEvent(
                        ConnEventKind.CONNECT_REQUEST, connection_id, auto_run_id=stop_event,
                    ))
                    connect = asyncio.create_task(self.connect_device(device, connection_id))
                    await asyncio.wait({connect, stop}, return_when=asyncio.FIRST_COMPLETED)
                    if stop_event.is_set():
                        connect.cancel()
                        with suppress(asyncio.CancelledError):
                            await connect
                        break
                    if await connect:
                        assert self._disconnect_evt is not None
                        disconnect = asyncio.create_task(self._disconnect_evt.wait())
                        try:
                            await asyncio.wait({disconnect, stop}, return_when=asyncio.FIRST_COMPLETED)
                        finally:
                            disconnect.cancel()
                            with suppress(asyncio.CancelledError):
                                await disconnect
                    else:
                        await asyncio.wait({stop}, timeout=0.4)
        finally:
            stop.cancel()
            with suppress(asyncio.CancelledError):
                await stop
            await self.disconnect_device(fast=True)
            self.connection_event.emit(ConnEvent(
                ConnEventKind.AUTO_STOPPED, auto_run_id=stop_event,
            ))

    @staticmethod
    def _load_saved_devices() -> list[DeviceInfo]:
        devices: list[DeviceInfo] = []
        records = load_paired_records()
        records.sort(key=lambda r: r.get("last_connected", 0), reverse=True)
        for item in records:
            address = item.get("address", "")
            if address:
                devices.append(DeviceInfo(name=item.get("name", "Unknown"), address=address))
        return devices

    async def _subscribe_runtime_status(
        self: BleWorker,
        name: str,
        subscribe: Callable[[Callable[[_Status], None]], Awaitable[None]],
        read: Callable[..., Awaitable[_Status]],
        signal: SignalInstance,
        connection_id: object,
    ) -> None:
        notification_count = 0

        def is_current() -> bool:
            return (self._connection_id is connection_id and self._disconnect_evt is not None
                    and not self._disconnect_evt.is_set())

        def on_notify(update: _Status) -> None:
            nonlocal notification_count
            if is_current():
                notification_count += 1
                signal.emit(update, connection_id)

        try:
            await self._with_timeout(subscribe(on_notify), f"start {name} notify", timeout=3.0)
        except Exception as exc:
            self.log.emit(self._conn_message(f"{name} notify unavailable: {exc}"))

        before_read = notification_count
        try:
            status = await read(timeout=self.config.metrics_timeout_s)
        except Exception as exc:
            self.log.emit(self._conn_message(f"Initial {name} read failed: {exc}"))
        else:
            if status is not None and before_read == notification_count and is_current():
                signal.emit(status, connection_id)

    async def connect_device(self: BleWorker, device: DeviceInfo, connection_id: object) -> bool:
        async with self._connection_lock:
            await self._disconnect_device(notify=False)
            self.connection_event.emit(ConnEvent(ConnEventKind.OLD_SESSION_CLOSED, connection_id))
            self._connection_id = connection_id
            disconnect_event = asyncio.Event()
            self._disconnect_evt = disconnect_event
            self.log.emit(self._conn_message(f"Connecting to {device.name} ({device.address})..."))
            try:
                loop = asyncio.get_running_loop()
                await self.core.connect_raw(
                    device, connect_timeout=self.config.connect_timeout_s,
                    disconnected_callback=lambda disconnected: loop.call_soon_threadsafe(
                        self._on_disconnected, connection_id, disconnected
                    ),
                )
                client = self.core.client
                if client is None:
                    raise RuntimeError("BLE client unavailable")
                self.log.emit(self._conn_message("Connected, starting AUTH..."))
                await self._do_auth()
                if disconnect_event.is_set() or not client.is_connected:
                    raise RuntimeError("Connection closed during authorization")
                self.log.emit(self._conn_message("AUTH ok, reading initial PARAMS..."))
                try:
                    params = await self.core.read_params(timeout=self.config.metrics_timeout_s)
                    self.params_received.emit(params)
                    self.log.emit(self._conn_message("Initial PARAMS read ok."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"Initial PARAMS read failed: {exc}"))
                self.log.emit(self._conn_message("AUTH ok, reading initial FAN status..."))
                try:
                    status = await self.core.read_fan_status(timeout=self.config.metrics_timeout_s)
                    self.fan_status_received.emit(status)
                    self.log.emit(self._conn_message("Initial FAN status read ok."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"Initial FAN status read failed: {exc}"))
                self.log.emit(self._conn_message("AUTH ok, reading initial OP status..."))
                try:
                    status = await self.core.read_operation_status(timeout=self.config.metrics_timeout_s)
                    if status.state == OP_STATE_IN_SERVICE:
                        self.operation_status_received.emit(status, connection_id)
                    self.log.emit(self._conn_message("Initial OP status read ok."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"Initial OP status read failed: {exc}"))
                self.log.emit(self._conn_message("AUTH ok, reading initial METRICS..."))
                try:
                    snapshot = await self.core.read_metrics_snapshot(timeout=self.config.metrics_timeout_s)
                    self._set_metrics_snapshot(snapshot)
                    self.log.emit(self._conn_message("Initial METRICS snapshot read ok."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"Initial METRICS snapshot read failed: {exc}"))
                self.log.emit(self._conn_message("Starting notify..."))
                try:
                    await self._with_timeout(
                        self.core.start_metrics_notify(
                            self._on_metrics_snapshot,
                            initial_snapshot=self._metrics_snapshot,
                        ),
                        "start_notify",
                    )
                    self.log.emit(self._conn_message("Notify METRICS started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"METRICS notify unavailable: {exc}"))
                try:
                    await self._with_timeout(
                        self.core.start_fan_status_notify(self._on_fan_status),
                        "start_fan_status_notify",
                        timeout=3.0,
                    )
                    self.log.emit(self._conn_message("Notify FAN status started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"FAN status notify unavailable: {exc}"))
                await self._subscribe_runtime_status(
                    "device status", self.core.start_device_status_notify, self.core.read_device_status,
                    self.device_status_received, connection_id,
                )
                try:
                    await self._with_timeout(
                        self.core.start_operation_status_notify(
                            lambda update: self._on_operation_status(update, connection_id)
                        ),
                        "start_operation_status_notify",
                        timeout=3.0,
                    )
                    self.log.emit(self._conn_message("Notify OP status started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"OP status notify unavailable: {exc}"))
                await self._subscribe_runtime_status(
                    "output limit", self.core.start_output_limit_notify, self.core.read_output_limit,
                    self.output_limit_received, connection_id,
                )
                await self._subscribe_runtime_status(
                    "fan mode", self.core.start_fan_mode_status_notify, self.core.read_fan_mode_status,
                    self.fan_mode_status_received, connection_id,
                )
                if disconnect_event.is_set() or not client.is_connected:
                    raise RuntimeError("Connection closed during setup")
                self.device = device
                self.connection_event.emit(ConnEvent(ConnEventKind.CONNECT_SUCCEEDED, connection_id, device))
                return True
            except asyncio.CancelledError:
                await self._disconnect_device()
                raise
            except Exception as exc:
                error = self._format_error(exc)
                self.log.emit(self._conn_message(f"Connection error: {error}"))
                await self._disconnect_device(notify=False)
                self.connection_event.emit(ConnEvent(ConnEventKind.CONNECT_FAILED, connection_id, error=error))
                return False

    async def disconnect_device(self: BleWorker, connection_id: object | None = None, *, fast: bool = False) -> None:
        async with self._connection_lock:
            if connection_id is None or self._connection_id is connection_id:
                await self._disconnect_device(fast=fast)

    async def _disconnect_device(self: BleWorker, *, notify: bool = True, fast: bool = False) -> None:
        connection_id = self._connection_id
        disconnected = self._disconnect_evt
        self._connection_id = None
        if self.core.client:
            if self.device is not None:
                with suppress(Exception):
                    await self.core.cancel_operation(timeout=1.0)
            if not fast:
                for stop_notify in (
                    self.core.stop_metrics_notify,
                    self.core.stop_fan_status_notify,
                    self.core.stop_device_status_notify,
                    self.core.stop_operation_status_notify,
                    self.core.stop_output_limit_notify,
                    self.core.stop_fan_mode_status_notify,
                ):
                    with suppress(Exception):
                        await self._with_timeout(stop_notify(), stop_notify.__name__, timeout=3.0)
            with suppress(Exception):
                await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
        self._metrics_snapshot = MetricsSnapshot.empty()
        self.device = None
        if disconnected is not None:
            disconnected.set()
        self._disconnect_evt = None
        if notify and connection_id is not None:
            self.connection_event.emit(ConnEvent(ConnEventKind.LINK_CLOSED, connection_id))

    async def pair(self: BleWorker, device: DeviceInfo) -> None:
        result = await self.core.pair(device)
        if result.ok:
            self.pairing_result.emit(True, "Device paired", device, result.k_hex)
            return
        self.pairing_result.emit(False, f"Pairing error: {result.message}", device, None)

    async def _do_auth(self: BleWorker) -> None:
        await self.core.auth(timeout=self.config.auth_timeout_s)

    async def write_params(self: BleWorker, params: DeviceParams) -> None:
        try:
            await self._with_timeout(
                self.core.write_params(params, timeout=self.config.metrics_timeout_s),
                "write_params",
                timeout=self.config.metrics_timeout_s,
            )
        except Exception as exc:
            self.log.emit(f"Failed to send parameters: {exc}")

    async def _run_params_command(self: BleWorker, command_id: object, command: Callable[[], Awaitable[None]]) -> None:
        try:
            await command()
            status = await self._with_timeout(
                self.core.read_params_status(timeout=self.config.metrics_timeout_s),
                "read_params_status", timeout=self.config.metrics_timeout_s,
            )
        except Exception as exc:
            error = f"Apply failed: {exc}"
            self.log.emit(error)
            self.params_command_done.emit(command_id, None, error)
        else:
            self.params_command_done.emit(command_id, status, "")

    async def apply_params(self: BleWorker, params: DeviceParams, command_id: object) -> None:
        async def apply() -> None:
            await self.core.write_params(params, timeout=self.config.metrics_timeout_s)
            await self.core.apply_params(timeout=self.config.metrics_timeout_s)

        await self._run_params_command(command_id, apply)

    async def start_fan_calibration(self: BleWorker) -> None:
        try:
            await self._with_timeout(
                self.core.start_fan_calibration(timeout=self.config.metrics_timeout_s),
                "fan_calibration",
                timeout=self.config.metrics_timeout_s,
            )
            self.log.emit("Calibration request sent")
        except Exception as exc:
            self.operation_start_failed.emit(OP_TYPE_FAN_CALIBRATION, self._format_error(exc))

    async def start_setup_fans(self: BleWorker) -> None:
        try:
            await self._with_timeout(
                self.core.start_setup_fans(timeout=self.config.metrics_timeout_s),
                "setup_fans",
                timeout=self.config.metrics_timeout_s,
            )
            self.log.emit("Setup fans request sent")
        except Exception as exc:
            self.operation_start_failed.emit(OP_TYPE_SETUP_FANS, self._format_error(exc))

    async def change_output_limit(self: BleWorker, command_id: object, maximum: Optional[int]) -> None:
        timeout = self.config.metrics_timeout_s
        connection_id = self._connection_id
        try:
            if maximum is None:
                await self.core.reset_output_limit(timeout=timeout)
            else:
                await self.core.set_output_max(maximum, timeout=timeout)
        except Exception as exc:
            self.output_limit_command_done.emit(command_id, self._format_error(exc))
        else:
            if maximum is not None and self._connection_id == connection_id:
                if not self.core.output_limit_notifications_active:
                    try:
                        status = await self.core.read_output_limit(timeout=timeout)
                    except Exception as exc:
                        self.log.emit(f"Maximum saved, but its status could not be read: {self._format_error(exc)}")
                    else:
                        self.output_limit_received.emit(status, connection_id)
                if self._connection_id == connection_id:
                    await self.read_params_snapshot()
            self.output_limit_command_done.emit(command_id, "")

    async def read_operation_status(self: BleWorker) -> None:
        connection_id = self._connection_id
        try:
            status = await self._with_timeout(
                self.core.read_operation_status(timeout=self.config.metrics_timeout_s),
                "read_operation_status",
                timeout=self.config.metrics_timeout_s,
            )
            self.operation_status_received.emit(status, connection_id)
            self.log.emit("OP status updated.")
        except Exception as exc:
            self.log.emit(f"Failed to read OP status: {exc}")

    async def read_params_snapshot(self: BleWorker) -> None:
        connection_id = self._connection_id
        try:
            params = await self._with_timeout(
                self.core.read_params(timeout=self.config.metrics_timeout_s),
                "read_params_snapshot",
                timeout=self.config.metrics_timeout_s,
            )
            if self._connection_id == connection_id:
                self.params_received.emit(params)
                self.log.emit("Parameters refreshed after operation.")
        except Exception as exc:
            self.log.emit(f"Failed to read parameters after operation: {exc}")

    async def read_profiles(self: BleWorker) -> None:
        try:
            slots = await self._with_timeout(
                self.core.read_profile_slots(timeout=self.config.metrics_timeout_s),
                "read_profiles",
                timeout=self.config.metrics_timeout_s,
            )
            metadata = {}
            for slot in range(1, PROFILE_COUNT + 1):
                metadata[slot] = await self._with_timeout(
                    self.core.read_profile_metadata(slot, timeout=self.config.metrics_timeout_s),
                    f"read_profile_metadata_{slot}",
                    timeout=self.config.metrics_timeout_s,
                )
            self.profiles_received.emit(slots)
            self.profile_metadata_received.emit(metadata)
        except Exception as exc:
            self.log.emit(f"Failed to read profiles: {exc}")

    async def _run_profile_action(self: BleWorker, action: int, slot: int) -> None:
        try:
            if action == PROFILE_ACTION_SAVE_CURRENT:
                await self._with_timeout(
                    self.core.save_profile(slot, timeout=self.config.metrics_timeout_s),
                    "save_profile",
                    timeout=self.config.metrics_timeout_s,
                )
            elif action == PROFILE_ACTION_CLEAR:
                await self._with_timeout(
                    self.core.clear_profile(slot, timeout=self.config.metrics_timeout_s),
                    "clear_profile",
                    timeout=self.config.metrics_timeout_s,
                )
            if action in (PROFILE_ACTION_SAVE_CURRENT, PROFILE_ACTION_CLEAR):
                slots = await self.core.read_profile_slots(timeout=self.config.metrics_timeout_s)
                self.profiles_received.emit(slots)
            self.profile_action_done.emit(action, slot, True, "")
        except Exception as exc:
            error = f"Profile {slot} operation failed: {exc}"
            self.log.emit(error)
            self.profile_action_done.emit(action, slot, False, error)

    async def save_profile(self: BleWorker, slot: int) -> None:
        await self._run_profile_action(PROFILE_ACTION_SAVE_CURRENT, slot)

    async def apply_profile(self: BleWorker, slot: int, command_id: object) -> None:
        timeout = self.config.metrics_timeout_s
        try:
            await self.core.apply_profile(slot, timeout=timeout)
            try:
                result = await self.core.read_profile_apply_result(slot, timeout=timeout)
            except BleakCharacteristicNotFoundError:
                result = await self.core.read_params_status(timeout=timeout)
        except Exception as exc:
            self.params_command_done.emit(command_id, None, self._format_error(exc))
        else:
            self.params_command_done.emit(command_id, result, "")

    async def clear_profile(self: BleWorker, slot: int) -> None:
        await self._run_profile_action(PROFILE_ACTION_CLEAR, slot)

    async def save_profile_metadata(
        self: BleWorker, slot: int, title: str, description: str
    ) -> None:
        try:
            await self._with_timeout(
                self.core.save_profile_metadata(
                    slot, title, description, timeout=self.config.metrics_timeout_s
                ),
                "save_profile_metadata",
                timeout=self.config.metrics_timeout_s,
            )
            metadata = await self.core.read_profile_metadata(
                slot, timeout=self.config.metrics_timeout_s
            )
            self.profile_metadata_done.emit(slot, metadata, True, "")
        except Exception as exc:
            error = f"Profile {slot} metadata update failed: {exc}"
            self.log.emit(error)
            self.profile_metadata_done.emit(slot, {}, False, error)

    def _on_metrics_snapshot(self: BleWorker, snapshot: MetricsSnapshot) -> None:
        self._set_metrics_snapshot(snapshot)

    def _on_fan_status(self: BleWorker, status: FanStatus) -> None:
        self.fan_status_received.emit(status)

    def _on_operation_status(self: BleWorker, status: OperationStatus, connection_id: object) -> None:
        self.operation_status_received.emit(status, connection_id)
