"""Visible window-boundary support for the frameless workbench profile."""

from __future__ import annotations

import ctypes
import sys
from typing import TYPE_CHECKING

from PySide6.QtCore import QEvent, QObject
from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import QFrame, QWidget

from .style import set_fluent_property
from .theme import qcolor
from .tokens import ResolvedTheme

if TYPE_CHECKING:
    from .theme import FluentThemeManager


DWMWA_WINDOW_CORNER_PREFERENCE = 33
DWMWA_BORDER_COLOR = 34
DWMWCP_ROUND = 2
DWMWA_COLOR_NONE = 0xFFFFFFFE
DWMWA_COLOR_DEFAULT = 0xFFFFFFFF


def windows_dwm_frame_supported() -> bool:
    """Return whether Windows supports DWM border and corner attributes."""
    if sys.platform != "win32":
        return False
    try:
        version = sys.getwindowsversion()
    except AttributeError:
        return False
    return version.major >= 10 and version.build >= 22000


def qcolor_to_colorref(color: QColor) -> int:
    """Convert a Qt color to the Win32 ``COLORREF`` byte order."""
    return color.red() | (color.green() << 8) | (color.blue() << 16)


class WindowsDwmFrameAdapter:
    """Apply a semantic outer border to a Windows 11 frameless HWND."""

    def __init__(self, window: QWidget, *, enabled: bool | None = None) -> None:
        self.window = window
        supported = windows_dwm_frame_supported()
        self.enabled = supported if enabled is None else bool(enabled and supported)
        self._setter = None
        if not self.enabled:
            return
        loader = getattr(ctypes, "WinDLL", None)
        if loader is None:
            self.enabled = False
            return
        try:
            setter = loader("dwmapi").DwmSetWindowAttribute
            setter.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint,
                ctypes.c_void_p,
                ctypes.c_uint,
            ]
            setter.restype = ctypes.c_long
            self._setter = setter
        except (AttributeError, OSError):
            self.enabled = False

    def apply(self, color: QColor | None) -> bool:
        """Apply ``color`` or suppress the border when it is ``None``."""
        if not self.enabled or self._setter is None:
            return False
        # Retain Windows 11 rounding while Qt owns the client title surface.
        self._set_attribute(DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND)
        value = DWMWA_COLOR_NONE if color is None else qcolor_to_colorref(color)
        return self._set_attribute(DWMWA_BORDER_COLOR, value)

    def reset(self) -> bool:
        """Restore the platform-selected border color."""
        return self._set_attribute(DWMWA_BORDER_COLOR, DWMWA_COLOR_DEFAULT)

    def _set_attribute(self, attribute: int, value: int) -> bool:
        if not self.enabled or self._setter is None:
            return False
        try:
            hwnd = int(self.window.winId())
        except (RuntimeError, TypeError, ValueError):
            return False
        native_value = ctypes.c_uint32(value)
        result = self._setter(
            ctypes.c_void_p(hwnd),
            ctypes.c_uint(attribute),
            ctypes.byref(native_value),
            ctypes.sizeof(native_value),
        )
        return int(result) == 0


class FluentWindowFrameController(QObject):
    """Keep a frameless window's semantic outer boundary in sync.

    Windows 11 receives a true DWM-drawn border. Other environments use the
    generated-QSS client border on ``frame``. The existing window move,
    resize, caption, and Snap Layout controllers remain independent.
    """

    def __init__(
        self,
        window: QWidget,
        frame: QFrame,
        *,
        theme_manager: FluentThemeManager | None = None,
    ) -> None:
        super().__init__(window)
        self.setObjectName("fluentWindowFrameController")
        self.window = window
        self.frame = frame
        self._theme: ResolvedTheme | None = None
        self.native_adapter = WindowsDwmFrameAdapter(window)

        if not frame.objectName():
            frame.setObjectName("fluentWindowFrame")
        set_fluent_property(frame, "fluentRole", "windowFrame")
        window.installEventFilter(self)

        if theme_manager is not None:
            theme_manager.themeChanged.connect(self.set_theme)
            self._theme = theme_manager.current_theme
        self._synchronize()

    @property
    def mode(self) -> str:
        """Return ``native`` when DWM owns the border, otherwise ``client``."""
        return str(self.frame.property("windowFrameMode") or "client")

    @property
    def active_border_color(self) -> QColor:
        return QColor(self._border_colors()[0])

    @property
    def inactive_border_color(self) -> QColor:
        return QColor(self._border_colors()[1])

    def set_theme(self, theme: ResolvedTheme) -> None:
        """Apply newly resolved semantic colors after a theme change."""
        self._theme = theme
        self._synchronize()

    def eventFilter(self, watched: object, event: QEvent) -> bool:  # noqa: N802 - Qt API
        if watched is self.window and event.type() in (
            QEvent.Type.Show,
            QEvent.Type.WinIdChange,
            QEvent.Type.WindowActivate,
            QEvent.Type.WindowDeactivate,
            QEvent.Type.WindowStateChange,
            QEvent.Type.PaletteChange,
            QEvent.Type.StyleChange,
        ):
            self._synchronize()
        return super().eventFilter(watched, event)

    def _border_colors(self) -> tuple[QColor, QColor]:
        if self._theme is not None:
            aliases = self._theme.aliases
            active = qcolor(
                aliases.get("shell_window_border_active", aliases["border_primary"])
            )
            inactive = qcolor(
                aliases.get("shell_window_border_inactive", aliases["border_subtle"])
            )
            return active, inactive

        palette = self.frame.palette()
        role = QPalette.ColorRole
        group = QPalette.ColorGroup
        return (
            palette.color(group.Active, role.Mid),
            palette.color(group.Inactive, role.Midlight),
        )

    def _synchronize(self) -> None:
        active = self.window.isActiveWindow()
        visible = not (self.window.isMaximized() or self.window.isFullScreen())
        active_color, inactive_color = self._border_colors()
        native = self.native_adapter.apply(
            (active_color if active else inactive_color) if visible else None
        )
        set_fluent_property(self.frame, "windowFrameMode", "native" if native else "client")
        set_fluent_property(self.frame, "windowFrameVisible", visible)
        set_fluent_property(self.frame, "windowActive", active)
