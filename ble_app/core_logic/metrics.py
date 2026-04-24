from __future__ import annotations

import asyncio
from typing import Callable, Optional

from .codec import decode_fan_speed, decode_temp_value
from .models import MetricsSnapshot
from .protocol import FAN_SPEED_UUIDS, TEMP_CHAR_UUIDS


class BleCoreMetricsMixin:
    async def _read_temperatures_once(self, timeout: float) -> tuple[float, float, float, float]:
        if not self.client:
            raise RuntimeError("Not connected")
        values: list[float] = []
        for uuid in TEMP_CHAR_UUIDS:
            data = await asyncio.wait_for(self.client.read_gatt_char(uuid), timeout=timeout)
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
            data = await asyncio.wait_for(self.client.read_gatt_char(uuid), timeout=timeout)
            value = decode_fan_speed(bytes(data))
            if value is None:
                raise RuntimeError(f"Bad fan speed value len={len(data)}")
            values.append(value)
        return values[0], values[1], values[2], values[3]

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
        return MetricsSnapshot(temperatures=temperatures, fan_speeds=fan_speeds)

    async def _reconnect_for_metrics(self, timeout: float) -> None:
        try:
            await self.disconnect()
        except Exception:
            pass
        await asyncio.sleep(self._config.metrics_reconnect_delay_s)
        if not self.device:
            return
        await self.connect_raw(
            self.device,
            connect_timeout=max(self._config.connect_timeout_s, timeout),
        )
        await self.auth(timeout=timeout)

    async def start_metrics_notify(
        self,
        callback: Callable[[MetricsSnapshot], None],
        initial_snapshot: Optional[MetricsSnapshot] = None,
    ) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        state = {"snapshot": initial_snapshot or MetricsSnapshot.empty()}
        for idx, uuid in enumerate(TEMP_CHAR_UUIDS):
            await self.client.start_notify(
                uuid,
                lambda _, data, ch=idx: self._emit_temp(callback, state, ch, data),
            )
        for idx, uuid in enumerate(FAN_SPEED_UUIDS):
            try:
                await self.client.start_notify(
                    uuid,
                    lambda _, data, ch=idx: self._emit_fan(callback, state, ch, data),
                )
            except Exception as exc:
                self._emit(f"Fan {idx + 1} notify unavailable: {exc}")

    async def stop_metrics_notify(self) -> None:
        if not self.client:
            raise RuntimeError("Not connected")
        for uuid in TEMP_CHAR_UUIDS:
            await self.client.stop_notify(uuid)
        for uuid in FAN_SPEED_UUIDS:
            try:
                await self.client.stop_notify(uuid)
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
