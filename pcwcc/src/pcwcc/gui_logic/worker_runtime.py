from __future__ import annotations

import asyncio
from concurrent.futures import Future
from typing import Callable, Optional

from ..presentation import ConnEvent, ConnEventKind


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

    def stop_auto(self, stop_event: asyncio.Event) -> None:
        if self.loop:
            self._call_soon_noargs(self.loop, stop_event.set)

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

    def _on_disconnected(self, connection_id: object, client) -> None:
        if self._connection_id is not connection_id or self.core.client is not client:
            return
        if self._disconnect_evt is None or self._disconnect_evt.is_set():
            return
        self._disconnect_evt.set()
        if self.device is not None:
            self.log.emit(self._conn_message("Connection dropped by device."))
            self.connection_event.emit(ConnEvent(ConnEventKind.LINK_CLOSED, connection_id))
