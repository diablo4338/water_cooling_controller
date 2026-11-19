from __future__ import annotations

import asyncio
from typing import Optional

from PySide6.QtCore import QTimer

from ..presentation import ConnEvent, ConnEventKind


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
        if action in (self.Action.APPLY, self.Action.APPLY_PROFILE, self.Action.SET_OUTPUT_MAX):
            steps = 2 if action == self.Action.APPLY_PROFILE else 3
            timeout_s = max(timeout_s, steps * self.worker.config.metrics_timeout_s)
        if use_timeout:
            timer = QTimer(self)
            timer.setSingleShot(True)
            timer.timeout.connect(lambda: self._action_timeout(action))
            timer.start(int(timeout_s * 1000))
            self._action_timers[action] = timer
        fut = self.worker.submit(coro)
        if fut is None:
            coro.close()
            state = self.model.state
            if action == self.Action.CONNECT:
                self.on_connection_event(ConnEvent(ConnEventKind.CONNECT_FAILED, state.connection_id,
                                                   error="BLE worker unavailable"))
            elif action == self.Action.DISCONNECT:
                self.on_connection_event(ConnEvent(ConnEventKind.LINK_CLOSED, state.connection_id))
            elif action == self.Action.AUTO_CONNECT:
                self.on_connection_event(ConnEvent(ConnEventKind.AUTO_STOPPED,
                                                   auto_run_id=state.auto_run_id))
            else:
                self._finish_action(action)
            return
        self._action_futures[action] = fut

    def _action_timeout(self, action) -> None:
        fut = self._action_futures.get(action)
        if fut and not fut.done():
            fut.cancel()
        self.on_log(f"Operation '{action.name}' exceeded send timeout.")
        if action in (self.Action.APPLY, self.Action.APPLY_PROFILE):
            self._show_apply_result("error", "Error: apply timed out")
        self._finish_action(action)

    def _finish_action(self, action) -> None:
        timer = self._action_timers.pop(action, None)
        if timer:
            timer.stop()
        self._action_futures.pop(action, None)
        self.model.finish_action(action)
        self._apply_ui()

    def _dispatch_action(self, action) -> None:
        if action == self.Action.APPLY:
            command = self.model.dispatch(action, slot=self._selected_profile_index)
        else:
            command = self.model.dispatch(action)
        self._apply_ui()
        if command is None:
            return
        if command.action == self.Action.SCAN:
            self._start_action(self.Action.SCAN, self.worker.scan(), use_timeout=False)
        elif command.action == self.Action.PAIR and command.device:
            self._start_action(self.Action.PAIR, self.worker.pair(command.device), use_timeout=False)
        elif command.action == self.Action.CONNECT and command.device:
            self._load_metrics_chart_history(command.device)
            self._start_action(
                self.Action.CONNECT,
                self.worker.connect_device(command.device, command.connection_id),
                timeout_override=self.action_timeouts[self.Action.CONNECT],
                use_timeout=False,
            )
        elif command.action == self.Action.DISCONNECT:
            self._save_metrics_chart_history()
            self._start_action(
                self.Action.DISCONNECT,
                self.worker.disconnect_device(command.connection_id),
                timeout_override=self.action_timeouts[self.Action.DISCONNECT],
                use_timeout=False,
            )
        elif command.action == self.Action.APPLY:
            self._start_action(self.Action.APPLY, self.worker.apply_params(self._current_params(), self.model.state.command_id))
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

    def on_rename_paired_clicked(self) -> None:
        self._rename_selected_paired()

    def on_auto_toggled(self, enabled: bool) -> None:
        if enabled:
            stop_event = asyncio.Event()
            if self.model.start_auto(stop_event):
                self._start_action(
                    self.Action.AUTO_CONNECT, self.worker.auto_connect_saved(stop_event),
                    use_timeout=False,
                )
        else:
            stop_event = self.model.state.auto_run_id
            if stop_event is not None:
                self.model.stop_auto()
                self.worker.stop_auto(stop_event)
                self.found_list.blockSignals(True)
                self.found_list.clearSelection()
                self.found_list.blockSignals(False)
                self.paired_list.blockSignals(True)
                self.paired_list.clearSelection()
                self.paired_list.blockSignals(False)
                self.model.set_selection(None, None)
        self._apply_ui()


    def on_change_output_limit(self, maximum: Optional[int]) -> None:
        action = self.Action.RESET_OUTPUT_LIMIT if maximum is None else self.Action.SET_OUTPUT_MAX
        if self.model.dispatch(action) is None:
            return
        self._apply_ui()
        self._start_action(action, self.worker.change_output_limit(self.model.state.command_id, maximum))
