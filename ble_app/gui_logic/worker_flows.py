from __future__ import annotations

import asyncio
from typing import Optional

from ..core import (
    DeviceInfo,
    DeviceParams,
    DeviceStatus,
    FanStatus,
    MetricsSnapshot,
    OP_STATE_IN_SERVICE,
    OperationStatus,
    ParamsStatus,
    load_paired_records,
)


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

    async def scan(self) -> None:
        self.log.emit("Scanning for devices in pairing mode...")
        try:
            devices = await self.core.scan_pairing(timeout=self.config.scan_timeout_s)
        except Exception as exc:
            self.log.emit(f"Scan error: {exc}")
            self.scan_results.emit([])
            return
        self.scan_results.emit(devices)
        self.log.emit(f"Pairing-ready devices found: {len(devices)}")

    async def auto_connect_saved(self) -> None:
        if self._auto_running:
            self.log.emit("Auto-connect is already running.")
            return
        self._auto_running = True
        self._auto_stop_evt = asyncio.Event()
        try:
            while not self._auto_stop_evt.is_set():
                saved = self._load_saved_devices()
                if not saved:
                    self.log.emit("No saved devices.")
                    try:
                        await asyncio.wait_for(self._auto_stop_evt.wait(), timeout=2.0)
                    except asyncio.TimeoutError:
                        pass
                    continue
                for device in saved:
                    if self._auto_stop_evt.is_set():
                        break
                    await self.connect_device(device)
                    if self.core.client:
                        await self._wait_disconnect_or_stop()
                    if self._auto_stop_evt.is_set():
                        break
        finally:
            self._auto_running = False
            self._auto_stop_evt = None

    async def _wait_disconnect_or_stop(self) -> None:
        if self._disconnect_evt is None or self._auto_stop_evt is None:
            return
        while True:
            if self._disconnect_evt.is_set() or self._auto_stop_evt.is_set():
                break
            client = self.core.client
            if client is None or not getattr(client, "is_connected", False):
                self._on_disconnected(None)
                break
            await asyncio.sleep(0.5)
        if self._auto_stop_evt.is_set():
            await self.disconnect_device()

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

    async def connect_device(self, device: DeviceInfo) -> None:
        await self.disconnect_device()
        self._metrics_snapshot = MetricsSnapshot.empty()
        self.log.emit(self._conn_message(f"Connecting to {device.name} ({device.address})..."))
        last_exc: Optional[Exception] = None
        self._disconnect_evt = asyncio.Event()
        for attempt in range(1, 2):
            self.log.emit(self._conn_message(f"Connect attempt {attempt} to {device.address}"))
            try:
                await self.core.connect_raw(device, connect_timeout=self.config.connect_timeout_s)
                if self.core.client and hasattr(self.core.client, "set_disconnected_callback"):
                    self.core.client.set_disconnected_callback(self._on_disconnected)
                self.log.emit(self._conn_message("Connected, starting AUTH..."))
                await self._do_auth()
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
                self.log.emit(self._conn_message("AUTH ok, reading initial DEVICE status..."))
                try:
                    status = await self.core.read_device_status(timeout=self.config.metrics_timeout_s)
                    self.device_status_received.emit(status)
                    self.log.emit(self._conn_message("Initial DEVICE status read ok."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"Initial DEVICE status read failed: {exc}"))
                self.log.emit(self._conn_message("AUTH ok, reading initial OP status..."))
                try:
                    status = await self.core.read_operation_status(timeout=self.config.metrics_timeout_s)
                    if status.state == OP_STATE_IN_SERVICE:
                        self.operation_status_received.emit(status)
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
                        f"start_notify#{attempt}",
                    )
                    self.log.emit(self._conn_message("Notify METRICS started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"METRICS notify unavailable: {exc}"))
                try:
                    await self._with_timeout(
                        self.core.start_params_notify(self._on_params_status),
                        f"start_params_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit(self._conn_message("Notify PARAMS status started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"PARAMS status notify unavailable: {exc}"))
                try:
                    await self._with_timeout(
                        self.core.start_fan_status_notify(self._on_fan_status),
                        f"start_fan_status_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit(self._conn_message("Notify FAN status started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"FAN status notify unavailable: {exc}"))
                try:
                    await self._with_timeout(
                        self.core.start_device_status_notify(self._on_device_status),
                        f"start_device_status_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit(self._conn_message("Notify DEVICE status started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"DEVICE status notify unavailable: {exc}"))
                try:
                    await self._with_timeout(
                        self.core.start_operation_status_notify(self._on_operation_status),
                        f"start_operation_status_notify#{attempt}",
                        timeout=3.0,
                    )
                    self.log.emit(self._conn_message("Notify OP status started."))
                except Exception as exc:
                    self.log.emit(self._conn_message(f"OP status notify unavailable: {exc}"))
                self._start_monitor()
                self.device = device
                self.connection_state.emit(True, device)
                return
            except Exception as exc:
                last_exc = exc
                self.log.emit(self._conn_message(
                    f"Connect attempt {attempt} failed: {self._format_error(exc)}"
                ))
                try:
                    await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
                except Exception:
                    pass
                await asyncio.sleep(0.4 * attempt)
        self.log.emit(self._conn_message(f"Connection error: {self._format_error(last_exc)}"))
        if self._disconnect_evt is None or not self._disconnect_evt.is_set():
            self.connection_state.emit(False, None)

    async def disconnect_device(self) -> None:
        already_disconnected = (
            self._disconnect_evt.is_set() if self._disconnect_evt is not None else False
        )
        self._manual_disconnect = True
        try:
            if self.core.client:
                log_enabled = not already_disconnected
                if log_enabled:
                    self.log.emit(self._conn_message("Disconnecting..."))
                if self._monitor_stop_evt is not None:
                    self._monitor_stop_evt.set()
                try:
                    if log_enabled:
                        self.log.emit(self._conn_message("Stopping notify METRICS..."))
                    await self._with_timeout(self.core.stop_metrics_notify(), "stop_notify", timeout=3.0)
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit(self._conn_message("Stopping notify PARAMS..."))
                    await self._with_timeout(self.core.stop_params_notify(), "stop_params_notify", timeout=3.0)
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit(self._conn_message("Stopping notify FAN status..."))
                    await self._with_timeout(
                        self.core.stop_fan_status_notify(),
                        "stop_fan_status_notify",
                        timeout=3.0,
                    )
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit(self._conn_message("Stopping notify DEVICE status..."))
                    await self._with_timeout(
                        self.core.stop_device_status_notify(),
                        "stop_device_status_notify",
                        timeout=3.0,
                    )
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit(self._conn_message("Stopping notify OP status..."))
                    await self._with_timeout(
                        self.core.stop_operation_status_notify(),
                        "stop_operation_status_notify",
                        timeout=3.0,
                    )
                except Exception:
                    pass
                try:
                    if log_enabled:
                        self.log.emit(self._conn_message("Disconnecting BLE..."))
                    await self._with_timeout(self.core.disconnect(), "disconnect", timeout=3.0)
                except Exception:
                    pass
            self._metrics_snapshot = MetricsSnapshot.empty()
            self.device = None
            if self._disconnect_evt is not None:
                self._disconnect_evt.set()
            if not already_disconnected:
                self.connection_state.emit(False, None)
        finally:
            self._manual_disconnect = False

    async def pair(self, device: DeviceInfo) -> None:
        result = await self.core.pair(device)
        if result.ok:
            self.pairing_result.emit(True, "Device paired", device, result.k_hex)
            return
        self.pairing_result.emit(False, f"Pairing error: {result.message}", device, None)

    async def _do_auth(self) -> None:
        await self.core.auth(timeout=self.config.auth_timeout_s)

    async def write_params(self, params: DeviceParams) -> None:
        try:
            await self._with_timeout(
                self.core.write_params(params, timeout=self.config.metrics_timeout_s),
                "write_params",
                timeout=self.config.metrics_timeout_s,
            )
        except Exception as exc:
            self.log.emit(f"Failed to send parameters: {exc}")

    async def apply_params(self) -> None:
        try:
            await self._with_timeout(
                self.core.apply_params(timeout=self.config.metrics_timeout_s),
                "apply_params",
                timeout=self.config.metrics_timeout_s,
            )
        except Exception as exc:
            self.log.emit(f"Apply failed: {exc}")
        finally:
            self.apply_done.emit()

    async def start_fan_calibration(self) -> None:
        try:
            await self._with_timeout(
                self.core.start_fan_calibration(timeout=self.config.metrics_timeout_s),
                "fan_calibration",
                timeout=self.config.metrics_timeout_s,
            )
            self.log.emit("Calibration request sent")
        except Exception as exc:
            self.log.emit(f"Fan calibration error: {exc}")

    async def start_setup_fans(self) -> None:
        try:
            await self._with_timeout(
                self.core.start_setup_fans(timeout=self.config.metrics_timeout_s),
                "setup_fans",
                timeout=self.config.metrics_timeout_s,
            )
            self.log.emit("Setup fans request sent")
        except Exception as exc:
            self.log.emit(f"Setup fans error: {exc}")

    async def read_operation_status(self) -> None:
        try:
            status = await self._with_timeout(
                self.core.read_operation_status(timeout=self.config.metrics_timeout_s),
                "read_operation_status",
                timeout=self.config.metrics_timeout_s,
            )
            self.operation_status_received.emit(status)
            self.log.emit("OP status updated.")
        except Exception as exc:
            self.log.emit(f"Failed to read OP status: {exc}")

    async def read_params_snapshot(self) -> None:
        try:
            params = await self._with_timeout(
                self.core.read_params(timeout=self.config.metrics_timeout_s),
                "read_params_snapshot",
                timeout=self.config.metrics_timeout_s,
            )
            self.params_received.emit(params)
            self.log.emit("Parameters refreshed after operation.")
        except Exception as exc:
            self.log.emit(f"Failed to read parameters after operation: {exc}")

    def _on_metrics_snapshot(self, snapshot: MetricsSnapshot) -> None:
        self._set_metrics_snapshot(snapshot)

    def _on_params_status(self, status: ParamsStatus) -> None:
        self.params_status.emit(status)

    def _on_fan_status(self, status: FanStatus) -> None:
        self.fan_status_received.emit(status)

    def _on_device_status(self, status: DeviceStatus) -> None:
        self.device_status_received.emit(status)

    def _on_operation_status(self, status: OperationStatus) -> None:
        self.operation_status_received.emit(status)
