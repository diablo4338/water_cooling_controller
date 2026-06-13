from __future__ import annotations

import asyncio
from concurrent.futures import Future
from typing import Callable, Optional

from ..core import UUID_CONFIG_STATUS


class BleWorkerRuntimeMixin:
    def run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    @staticmethod
    def _call_soon_noargs(loop: asyncio.AbstractEventLoop, fn: Callable[[], None]) -> None:
        def _cb(_: object) -> None:
            fn()

        loop.call_soon_threadsafe(_cb, None)

    def stop(self) -> None:
        if self.loop:
            self._call_soon_noargs(self.loop, self.loop.stop)

    def stop_auto(self) -> None:
        if self.loop and self._auto_stop_evt is not None:
            self._call_soon_noargs(self.loop, self._auto_stop_evt.set)

    def submit(self, coro) -> Optional[Future]:
        if not self.loop:
            return None
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    def _emit_metrics_snapshot(self) -> None:
        self.metrics_received.emit(self._metrics_snapshot)

    def _set_metrics_snapshot(self, snapshot) -> None:
        self._metrics_snapshot = snapshot
        self._emit_metrics_snapshot()

    async def _with_timeout(self, coro, label: str, timeout: Optional[float] = None):
        try:
            use_timeout = self.config.gui_action_default_timeout_s if timeout is None else timeout
            return await asyncio.wait_for(coro, timeout=use_timeout)
        except asyncio.TimeoutError as exc:
            raise RuntimeError(f"Timeout: {label}") from exc

    def _on_disconnected(self, _) -> None:
        if self._disconnect_evt is not None and self._disconnect_evt.is_set():
            return
        if self._disconnect_evt is not None:
            self._disconnect_evt.set()
        if self._monitor_stop_evt is not None:
            self._monitor_stop_evt.set()
        if self._manual_disconnect:
            return
        self.log.emit(self._conn_message("Connection dropped by device."))
        self.connection_state.emit(False, None)

    def _start_monitor(self) -> None:
        if self._monitor_task is not None:
            return
        self._monitor_stop_evt = asyncio.Event()
        self._monitor_task = asyncio.create_task(self._monitor_connection())

    async def _monitor_connection(self) -> None:
        try:
            ping_interval = 2.0
            ping_every = 3
            counter = 0
            while self._monitor_stop_evt is not None and not self._monitor_stop_evt.is_set():
                client = self.core.client
                if client is None:
                    break
                if not getattr(client, "is_connected", False):
                    self._on_disconnected(None)
                    break
                counter += 1
                if counter >= ping_every:
                    counter = 0
                    try:
                        await asyncio.wait_for(client.read_gatt_char(UUID_CONFIG_STATUS), timeout=1.0)
                    except Exception:
                        self._on_disconnected(None)
                        break
                await asyncio.sleep(ping_interval)
        finally:
            self._monitor_task = None
            self._monitor_stop_evt = None
