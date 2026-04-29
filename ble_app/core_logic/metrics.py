from __future__ import annotations

import asyncio
from typing import Callable, Optional

from .codec import decode_fan_speed, decode_power_metric, decode_temp_value
from .models import MetricsSnapshot
from .protocol import FAN_SPEED_UUIDS, TEMP_CHAR_UUIDS, UUID_CURRENT_VALUE, UUID_VOLTAGE_VALUE


class BleCoreMetricsMixin:
    async def _read_temperatures_once(self, timeout: float) -> tuple[float, float, float, float]:
        if not self.client:
            raise RuntimeError("Not connected")
        values: list[float] = []
        for uuid in TEMP_CHAR_UUIDS:
            data = await self._read_char(uuid, timeout=timeout)
            value = decode_temp_value(bytes(data))
            if value is None:
                raise RuntimeError(f"Bad metrics value len={len(data)}")
            values.append(value)
        return values[0], values[1], values[2], values[3]

    async def _read_fan_speeds(self, timeout: Optional[float] = None) -> tuple[float, float, float, float]:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        values: list[float] = []
        for uuid in FAN_SPEED_UUIDS:
            data = await self._read_char(uuid, timeout=timeout)
            value = decode_fan_speed(bytes(data))
            if value is None:
                raise RuntimeError(f"Bad fan speed value len={len(data)}")
            values.append(value)
        return values[0], values[1], values[2], values[3]

    async def _read_power_metrics(self, timeout: Optional[float] = None) -> tuple[float | None, float | None]:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        try:
            voltage_data = await self._read_char(UUID_VOLTAGE_VALUE, timeout=timeout)
            current_data = await self._read_char(UUID_CURRENT_VALUE, timeout=timeout)
        except Exception as exc:
            self._emit(f"Power metrics unavailable: {exc}")
            return None, None
        voltage = decode_power_metric(bytes(voltage_data))
        current = decode_power_metric(bytes(current_data))
        if voltage is None or current is None:
            self._emit("Power metrics returned bad payload length")
            return None, None
        return voltage, current

    async def read_metrics_snapshot(
        self, timeout: Optional[float] = None, retries: Optional[int] = None
    ) -> MetricsSnapshot:
        if not self.client:
            raise RuntimeError("Not connected")
        if timeout is None:
            timeout = self._config.metrics_timeout_s
        if retries is None:
            retries = self._config.metrics_retries
        last_exc: Optional[Exception] = None
        for attempt in range(retries + 1):
            try:
                return await self._read_metrics_snapshot_once(timeout)
            except Exception as exc:
                last_exc = exc
                self._emit(
                    f"Metrics snapshot read failed (attempt {attempt + 1}/{retries + 1}): {exc}"
                )
                if attempt >= retries or not self.device:
                    break
                await self._reconnect_for_metrics(timeout)
        assert last_exc is not None
        raise last_exc

    async def _read_metrics_snapshot_once(self, timeout: float) -> MetricsSnapshot:
        temperatures = await self._read_temperatures_once(timeout)
        fan_speeds = await self._read_fan_speeds(timeout=timeout)
        voltage, current = await self._read_power_metrics(timeout=timeout)
        return MetricsSnapshot(
            temperatures=temperatures,
            fan_speeds=fan_speeds,
            voltage_v=voltage,
            current_ma=current,
        )

    async def _reconnect_for_metrics(self, timeout: float) -> None:
        self._emit_conn("metrics reconnect: start")
        try:
            await self.disconnect()
        except Exception:
            pass
        self._emit_conn(
            f"metrics reconnect: sleeping {self._config.metrics_reconnect_delay_s:.1f}s before reconnect"
        )
        await self._run_step(
            "metrics reconnect delay",
            asyncio.sleep(self._config.metrics_reconnect_delay_s),
            timeout=self._config.metrics_reconnect_delay_s + 0.5,
        )
        if not self.device:
            return
        await self.connect_raw(
            self.device,
            connect_timeout=max(self._config.connect_timeout_s, timeout),
        )
        await self.auth(timeout=timeout)
        self._emit_conn("metrics reconnect: completed")

    async def start_metrics_notify(
        self,
        callback: Callable[[MetricsSnapshot], None],
        initial_snapshot: Optional[MetricsSnapshot] = None,
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        state = {"snapshot": initial_snapshot or MetricsSnapshot.empty()}
        for idx, uuid in enumerate(TEMP_CHAR_UUIDS):
            await self._start_notify(
                uuid,
                lambda _, data, ch=idx: self._emit_temp(callback, state, ch, data),
            )
        for idx, uuid in enumerate(FAN_SPEED_UUIDS):
            try:
                await self._start_notify(
                    uuid,
                    lambda _, data, ch=idx: self._emit_fan(callback, state, ch, data),
                )
            except Exception as exc:
                self._emit(f"Fan {idx + 1} notify unavailable: {exc}")
        try:
            await self._start_notify(
                UUID_VOLTAGE_VALUE,
                lambda _, data: self._emit_voltage(callback, state, data),
            )
        except Exception as exc:
            self._emit(f"Voltage notify unavailable: {exc}")
        try:
            await self._start_notify(
                UUID_CURRENT_VALUE,
                lambda _, data: self._emit_current(callback, state, data),
            )
        except Exception as exc:
            self._emit(f"Current notify unavailable: {exc}")

    async def stop_metrics_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        for uuid in TEMP_CHAR_UUIDS:
            await self._stop_notify(uuid)
        for uuid in FAN_SPEED_UUIDS:
            try:
                await self._stop_notify(uuid)
            except Exception:
                pass
        for uuid in (UUID_VOLTAGE_VALUE, UUID_CURRENT_VALUE):
            try:
                await self._stop_notify(uuid)
            except Exception:
                pass

    @staticmethod
    def _emit_temp(
        callback: Callable[[MetricsSnapshot], None],
        state: dict[str, MetricsSnapshot],
        channel: int,
        data: bytearray,
    ) -> None:
        value = decode_temp_value(bytes(data))
        if value is None:
            return
        state["snapshot"] = state["snapshot"].with_temperature(channel, value)
        callback(state["snapshot"])

    @staticmethod
    def _emit_fan(
        callback: Callable[[MetricsSnapshot], None],
        state: dict[str, MetricsSnapshot],
        channel: int,
        data: bytearray,
    ) -> None:
        value = decode_fan_speed(bytes(data))
        if value is None:
            return
        state["snapshot"] = state["snapshot"].with_fan_speed(channel, value)
        callback(state["snapshot"])

    @staticmethod
    def _emit_voltage(
        callback: Callable[[MetricsSnapshot], None],
        state: dict[str, MetricsSnapshot],
        data: bytearray,
    ) -> None:
        value = decode_power_metric(bytes(data))
        if value is None:
            return
        state["snapshot"] = state["snapshot"].with_voltage(value)
        callback(state["snapshot"])

    @staticmethod
    def _emit_current(
        callback: Callable[[MetricsSnapshot], None],
        state: dict[str, MetricsSnapshot],
        data: bytearray,
    ) -> None:
        value = decode_power_metric(bytes(data))
        if value is None:
            return
        state["snapshot"] = state["snapshot"].with_current(value)
        callback(state["snapshot"])
