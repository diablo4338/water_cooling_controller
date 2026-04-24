from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QTimer


class MainWindowActionMixin:
    def _start_action(
        self,
        action,
        coro,
        timeout_override: Optional[float] = None,
        use_timeout: bool = True,
    ) -> None:
        timeout_s = (
            timeout_override
            if timeout_override is not None
            else self.action_timeouts.get(action, self.default_action_timeout)
        )
        if use_timeout:
            timer = QTimer(self)
            timer.setSingleShot(True)
            timer.timeout.connect(lambda: self._action_timeout(action))
            timer.start(int(timeout_s * 1000))
            self._action_timers[action] = timer
        fut = self.worker.submit(coro)
        if fut is None:
            self._finish_action(action)
            return
        self._action_futures[action] = fut

    def _action_timeout(self, action) -> None:
        fut = self._action_futures.get(action)
        if fut and not fut.done():
            fut.cancel()
        self.on_log(f"Operation '{action.name}' exceeded send timeout.")
        self._finish_action(action)

    def _finish_action(self, action) -> None:
        timer = self._action_timers.pop(action, None)
        if timer:
            timer.stop()
        self._action_futures.pop(action, None)
        self.model.finish_action(action)
        self._apply_ui()

    def _dispatch_action(self, action) -> None:
        command = self.model.dispatch(action)
        self._apply_ui()
        if command is None:
            return
        if command.action == self.Action.SCAN:
            self._start_action(self.Action.SCAN, self.worker.scan())
        elif command.action == self.Action.PAIR and command.device:
            self._start_action(self.Action.PAIR, self.worker.pair(command.device))
        elif command.action == self.Action.CONNECT and command.device:
            self.model.start_connecting()
            self._apply_ui()
            self._start_action(
                self.Action.CONNECT,
                self.worker.connect_device(command.device),
                timeout_override=self.action_timeouts[self.Action.CONNECT],
            )
        elif command.action == self.Action.DISCONNECT:
            self._start_action(
                self.Action.DISCONNECT,
                self.worker.disconnect_device(),
                timeout_override=self.action_timeouts[self.Action.DISCONNECT],
            )
        elif command.action == self.Action.APPLY:
            self._start_action(self.Action.APPLY, self.worker.apply_params())
        elif command.action == self.Action.DISCARD:
            self._start_action(self.Action.DISCARD, self.worker.read_params_snapshot())
        elif command.action == self.Action.CALIBRATE:
            self._start_action(self.Action.CALIBRATE, self.worker.start_fan_calibration(), use_timeout=False)
        elif command.action == self.Action.SETUP_FANS:
            self._start_action(self.Action.SETUP_FANS, self.worker.start_setup_fans(), use_timeout=False)
        elif command.action == self.Action.DELETE_PAIRED:
            self._delete_selected_paired()

    def on_scan(self) -> None:
        self._dispatch_action(self.Action.SCAN)

    def on_pair(self) -> None:
        self._dispatch_action(self.Action.PAIR)

    def on_connect(self) -> None:
        self._dispatch_action(self.Action.CONNECT)

    def on_disconnect(self) -> None:
        self._dispatch_action(self.Action.DISCONNECT)

    def on_apply(self) -> None:
        self._dispatch_action(self.Action.APPLY)

    def on_discard(self) -> None:
        self._dispatch_action(self.Action.DISCARD)

    def on_calibrate(self) -> None:
        self._clear_op_log()
        self._dispatch_action(self.Action.CALIBRATE)

    def on_setup_fans(self) -> None:
        self._clear_op_log()
        self._dispatch_action(self.Action.SETUP_FANS)

    def on_delete_paired_clicked(self) -> None:
        self._dispatch_action(self.Action.DELETE_PAIRED)

    def on_auto_toggled(self, enabled: bool) -> None:
        if enabled:
            if not self.model.set_auto_enabled(True):
                self._apply_ui()
                return
            self._apply_ui()
            self._start_action(self.Action.AUTO_CONNECT, self.worker.auto_connect_saved(), use_timeout=False)
            return
        self.worker.stop_auto()
        self.model.set_auto_enabled(False)
        self.found_list.blockSignals(True)
        self.found_list.clearSelection()
        self.found_list.blockSignals(False)
        if self.model.state.conn == self.ConnState.CONNECTED:
            self._lock_selection_to_connected()
        else:
            self.paired_list.blockSignals(True)
            self.paired_list.clearSelection()
            self.paired_list.blockSignals(False)
            self.model.set_selection(None, None)
        fut = self._action_futures.pop(self.Action.AUTO_CONNECT, None)
        if fut and not fut.done():
            fut.cancel()
        if self.model.state.conn != self.ConnState.DISCONNECTED:
            self._dispatch_action(self.Action.DISCONNECT)
            return
        self._apply_ui()
