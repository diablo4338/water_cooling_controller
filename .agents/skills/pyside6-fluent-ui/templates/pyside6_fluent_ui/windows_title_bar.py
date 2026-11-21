"""Optional Windows 11 Snap Layout support for the frameless title-bar profile."""

from __future__ import annotations

import ctypes
import sys

from PySide6.QtCore import QByteArray, QPoint, QPointF, QTimer, Qt
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QToolButton, QWidget

from .style import set_fluent_property


WM_NCHITTEST = 0x0084
WM_NCMOUSEMOVE = 0x00A0
WM_NCLBUTTONDOWN = 0x00A1
WM_NCLBUTTONUP = 0x00A2
WM_NCMOUSELEAVE = 0x02A2
WM_CANCELMODE = 0x001F
HTMAXBUTTON = 9


class _NativePoint(ctypes.Structure):
    _fields_ = (("x", ctypes.c_long), ("y", ctypes.c_long))


class _NativeMessage(ctypes.Structure):
    _fields_ = (
        ("hwnd", ctypes.c_void_p),
        ("message", ctypes.c_uint),
        ("wParam", ctypes.c_size_t),
        ("lParam", ctypes.c_ssize_t),
        ("time", ctypes.c_uint),
        ("pt", _NativePoint),
        ("lPrivate", ctypes.c_uint),
    )


def windows_snap_layout_supported() -> bool:
    """Return whether this process is running on Windows 11 or newer."""
    if sys.platform != "win32":
        return False
    version = sys.getwindowsversion()
    return version.major >= 10 and version.build >= 22000


def _signed_word(value: int) -> int:
    return ctypes.c_short(value & 0xFFFF).value


def screen_point_from_lparam(lparam: int) -> QPoint:
    """Decode signed virtual-screen coordinates from a Win32 message lParam."""
    return QPoint(_signed_word(lparam), _signed_word(lparam >> 16))


class WindowsSnapLayoutWindowMixin:
    """Route top-level native messages to an attached Snap Layout adapter."""

    def nativeEvent(  # noqa: N802 - Qt API
        self,
        event_type: QByteArray,
        message: int,
    ) -> tuple[bool, int]:
        adapter = getattr(self, "windows_snap_layout_adapter", None)
        if adapter is not None:
            handled, result = adapter.handle_native_event(event_type, message)
            if handled:
                return handled, result
        return super().nativeEvent(event_type, message)  # type: ignore[misc]


class WindowsSnapLayoutAdapter:
    """Expose a custom Qt maximize button as ``HTMAXBUTTON`` on Windows 11.

    The adapter intentionally owns only the maximize-button hit result. Every
    other native message is returned to Qt, so ``FramelessWindowController``
    remains responsible for client-side edge and corner resize targets.
    """

    EVENT_TYPES = {b"windows_generic_MSG", b"windows_dispatcher_MSG"}

    def __init__(
        self,
        window: QWidget,
        maximize_button: QToolButton,
        *,
        enabled: bool | None = None,
    ) -> None:
        self.window = window
        self.maximize_button = maximize_button
        supported = windows_snap_layout_supported()
        requested = supported if enabled is None else bool(enabled)
        has_maximize_hint = bool(
            window.windowFlags() & Qt.WindowType.WindowMaximizeButtonHint
        )
        has_native_route = isinstance(window, WindowsSnapLayoutWindowMixin)
        self.enabled = bool(
            requested and supported and has_maximize_hint and has_native_route
        )
        self._installed = self.enabled
        self._native_hover = False
        self._pending_native_hover = False
        self._pending_button_down = False
        self._pending_button_activation = False
        self._window_id = int(window.winId()) if self.enabled else 0
        self._hover_apply_timer = QTimer(window)
        self._hover_apply_timer.setSingleShot(True)
        self._hover_apply_timer.timeout.connect(self._apply_pending_native_hover)
        self._hover_timer = QTimer(window)
        self._hover_timer.setInterval(80)
        self._hover_timer.timeout.connect(self._sync_native_hover)
        self._interaction_timer = QTimer(window)
        self._interaction_timer.setSingleShot(True)
        self._interaction_timer.timeout.connect(self._apply_pending_interaction)

    @property
    def installed(self) -> bool:
        return self._installed

    def dispose(self, *_args: object) -> None:
        """Disable the adapter and clear its deferred interaction state."""
        try:
            self._hover_apply_timer.stop()
            self._hover_timer.stop()
            self._interaction_timer.stop()
        except RuntimeError:
            pass
        self._installed = False
        self._pending_native_hover = False
        self._pending_button_down = False
        self._pending_button_activation = False
        self._set_native_hover(False)

    def hit_test_client_position(self, position: QPointF | QPoint) -> int | None:
        """Return the native hit result for a Qt client-area position."""
        if self.maximize_button.isHidden() or not self.maximize_button.isEnabled():
            return None
        point = position.toPoint() if isinstance(position, QPointF) else position
        local = self.maximize_button.mapFrom(self.window, point)
        if self.maximize_button.rect().contains(local):
            return HTMAXBUTTON
        return None

    def handle_native_event(
        self,
        event_type: QByteArray,
        message: int,
    ) -> tuple[bool, int]:
        if not self.enabled or bytes(event_type) not in self.EVENT_TYPES:
            return False, 0

        try:
            native_message = ctypes.cast(
                int(message), ctypes.POINTER(_NativeMessage)
            ).contents
        except (TypeError, ValueError, RuntimeError):
            return False, 0

        if int(native_message.hwnd or 0) != self._window_id:
            return False, 0
        if native_message.message in (WM_NCMOUSELEAVE, WM_CANCELMODE):
            self._queue_native_hover(False)
            if native_message.message == WM_CANCELMODE:
                self._pending_button_activation = False
            self._queue_button_interaction(down=False)
            return False, 0
        if native_message.message == WM_NCMOUSEMOVE:
            if int(native_message.wParam) == HTMAXBUTTON:
                self._queue_native_hover(True)
            return False, 0
        if (
            native_message.message in (WM_NCLBUTTONDOWN, WM_NCLBUTTONUP)
            and int(native_message.wParam) == HTMAXBUTTON
        ):
            pressed = native_message.message == WM_NCLBUTTONDOWN
            self._queue_button_interaction(
                down=pressed,
                activate=not pressed,
            )
            return True, 0
        if native_message.message != WM_NCHITTEST:
            return False, 0

        client_position = self._client_position_from_lparam(
            self._window_id, native_message.lParam
        )
        result = self.hit_test_client_position(client_position)
        self._queue_native_hover(result == HTMAXBUTTON)
        if result == HTMAXBUTTON:
            if not self._hover_timer.isActive():
                self._hover_timer.start()
            return True, HTMAXBUTTON
        return False, 0

    def _client_position_from_lparam(self, hwnd: int, lparam: int) -> QPointF:
        screen_position = screen_point_from_lparam(lparam)
        if sys.platform != "win32":
            return QPointF(screen_position)

        point = _NativePoint(screen_position.x(), screen_position.y())
        user32 = ctypes.windll.user32
        if not user32.ScreenToClient(ctypes.c_void_p(hwnd), ctypes.byref(point)):
            return QPointF(screen_position)
        dpi_getter = getattr(user32, "GetDpiForWindow", None)
        dpi = int(dpi_getter(ctypes.c_void_p(hwnd))) if dpi_getter else 96
        scale = max(1.0, dpi / 96.0)
        return QPointF(point.x / scale, point.y / scale)

    def _sync_native_hover(self) -> None:
        try:
            local = self.maximize_button.mapFromGlobal(QCursor.pos())
            hovering = (
                self.maximize_button.isVisible()
                and self.maximize_button.isEnabled()
                and self.maximize_button.rect().contains(local)
            )
        except RuntimeError:
            hovering = False
        self._queue_native_hover(hovering)
        if not hovering:
            self._hover_timer.stop()

    def _queue_native_hover(self, hovering: bool) -> None:
        """Defer Qt repolishing until native window-message handling has returned."""
        self._pending_native_hover = hovering
        if not self._hover_apply_timer.isActive():
            self._hover_apply_timer.start(0)

    def _apply_pending_native_hover(self) -> None:
        self._set_native_hover(self._pending_native_hover)

    def _queue_button_interaction(
        self,
        *,
        down: bool,
        activate: bool = False,
    ) -> None:
        self._pending_button_down = down
        self._pending_button_activation = (
            self._pending_button_activation or activate
        )
        if not self._interaction_timer.isActive():
            self._interaction_timer.start(0)

    def _apply_pending_interaction(self) -> None:
        try:
            self.maximize_button.setDown(self._pending_button_down)
            if self._pending_button_activation:
                self._pending_button_activation = False
                self.maximize_button.click()
        except RuntimeError:
            self._pending_button_activation = False

    def _set_native_hover(self, hovering: bool) -> None:
        if self._native_hover == hovering:
            return
        self._native_hover = hovering
        try:
            set_fluent_property(self.maximize_button, "nativeHover", hovering)
        except RuntimeError:
            pass
