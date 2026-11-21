"""Fluent title surfaces for native, expanded, and single-row frameless windows."""

from __future__ import annotations

from enum import Enum
from pathlib import Path

from PySide6.QtCore import QByteArray, QEvent, QMargins, QObject, QPoint, QRectF, Qt
from PySide6.QtGui import (
    QAction,
    QContextMenuEvent,
    QFocusEvent,
    QIcon,
    QKeySequence,
    QMouseEvent,
    QPainter,
    QPalette,
    QPixmap,
    QResizeEvent,
    QShortcut,
    QShowEvent,
    QWindow,
)
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import (
    QAbstractButton,
    QAbstractSlider,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QMenuBar,
    QSizePolicy,
    QSpinBox,
    QStyle,
    QToolButton,
    QWidget,
)

from .metrics import ICON_SIZE_SMALL, SPACE_S, SPACE_S_NUDGE, SPACE_XS, SPACE_XXS
from .style import apply_accessible_identity, set_fluent_property


class TitleBarMode(str, Enum):
    """Supported window/title-surface integration profiles."""

    NATIVE_FALLBACK = "native-fallback"
    EXPANDED_CLIENT_AREA = "expanded-client-area"
    FRAMELESS = "frameless"


def _window_flag(name: str) -> object | None:
    enum = getattr(Qt, "WindowType", None)
    if enum is not None and hasattr(enum, name):
        return getattr(enum, name)
    return getattr(Qt, name, None)


def expanded_client_area_supported() -> bool:
    return _window_flag("ExpandedClientAreaHint") is not None and _window_flag(
        "NoTitleBarBackgroundHint"
    ) is not None


def enable_expanded_client_area(window: QWidget) -> bool:
    """Request Qt 6.9+ expanded client area before the window is first shown."""
    expanded = _window_flag("ExpandedClientAreaHint")
    no_background = _window_flag("NoTitleBarBackgroundHint")
    if expanded is None or no_background is None:
        return False
    window.setWindowFlags(window.windowFlags() | expanded | no_background)  # type: ignore[operator]
    window.setProperty("fluentWindowMode", TitleBarMode.EXPANDED_CLIENT_AREA.value)
    return True


def enable_frameless_window(window: QWidget) -> bool:
    """Enable the custom-caption profile before the window is first shown."""
    frameless = _window_flag("FramelessWindowHint")  # fluent-audit: allow title-bar profile
    if frameless is None:
        return False
    window.setWindowFlags(window.windowFlags() | frameless)  # type: ignore[operator]
    window.setProperty("fluentWindowMode", TitleBarMode.FRAMELESS.value)
    return True


class _ResizeHandle(QWidget):
    """Invisible client-edge target that delegates resizing to the window manager."""

    def __init__(
        self,
        window: QWidget,
        edges: Qt.Edge,
        cursor: Qt.CursorShape,
        name: str,
    ) -> None:
        super().__init__(window)
        self._window = window
        self._edges = edges
        self.setObjectName(f"framelessResizeHandle{name}")
        self.setCursor(cursor)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt API
        if event.button() == Qt.MouseButton.LeftButton:
            handle = self._window.windowHandle()
            start_resize = getattr(handle, "startSystemResize", None) if handle else None
            if callable(start_resize) and start_resize(self._edges):
                event.accept()
                return
        super().mousePressEvent(event)


class _CaptionButton(QToolButton):
    """Caption action that exposes a keyboard-only focus-visible property."""

    _KEYBOARD_REASONS = {
        Qt.FocusReason.TabFocusReason,
        Qt.FocusReason.BacktabFocusReason,
        Qt.FocusReason.ShortcutFocusReason,
    }

    def focusInEvent(self, event: QFocusEvent) -> None:  # noqa: N802 - Qt API
        set_fluent_property(self, "keyboardFocus", event.reason() in self._KEYBOARD_REASONS)
        super().focusInEvent(event)

    def focusOutEvent(self, event: QFocusEvent) -> None:  # noqa: N802 - Qt API
        set_fluent_property(self, "keyboardFocus", False)
        super().focusOutEvent(event)


class FramelessWindowController(QObject):
    """Restore Qt-native edge/corner resizing for a frameless top-level widget."""

    EDGE_SIZE = 6
    CORNER_SIZE = 12

    def __init__(self, window: QWidget) -> None:
        super().__init__(window)
        self.setObjectName("framelessWindowController")
        self.window = window
        top = Qt.Edge.TopEdge
        bottom = Qt.Edge.BottomEdge
        left = Qt.Edge.LeftEdge
        right = Qt.Edge.RightEdge
        definitions = {
            "TopLeft": (top | left, Qt.CursorShape.SizeFDiagCursor),
            "Top": (top, Qt.CursorShape.SizeVerCursor),
            "TopRight": (top | right, Qt.CursorShape.SizeBDiagCursor),
            "Left": (left, Qt.CursorShape.SizeHorCursor),
            "Right": (right, Qt.CursorShape.SizeHorCursor),
            "BottomLeft": (bottom | left, Qt.CursorShape.SizeBDiagCursor),
            "Bottom": (bottom, Qt.CursorShape.SizeVerCursor),
            "BottomRight": (bottom | right, Qt.CursorShape.SizeFDiagCursor),
        }
        self.handles = {
            name: _ResizeHandle(window, edges, cursor, name)
            for name, (edges, cursor) in definitions.items()
        }
        window.installEventFilter(self)
        self._update_handles()

    def eventFilter(self, watched: object, event: QEvent) -> bool:  # noqa: N802 - Qt API
        if watched is self.window and event.type() in (
            QEvent.Type.Resize,
            QEvent.Type.Show,
            QEvent.Type.WindowStateChange,
        ):
            self._update_handles()
        return super().eventFilter(watched, event)

    def _update_handles(self) -> None:
        maximized = self.window.isMaximized() or self.window.isFullScreen()
        for handle in self.handles.values():
            handle.setVisible(not maximized)
        if maximized:
            return

        width = self.window.width()
        height = self.window.height()
        edge = self.EDGE_SIZE
        corner = min(self.CORNER_SIZE, max(0, width // 2), max(0, height // 2))
        middle_width = max(0, width - 2 * corner)
        middle_height = max(0, height - 2 * corner)
        geometries = {
            "TopLeft": (0, 0, corner, corner),
            "Top": (corner, 0, middle_width, edge),
            "TopRight": (max(0, width - corner), 0, corner, corner),
            "Left": (0, corner, edge, middle_height),
            "Right": (max(0, width - edge), corner, edge, middle_height),
            "BottomLeft": (0, max(0, height - corner), corner, corner),
            "Bottom": (corner, max(0, height - edge), middle_width, edge),
            "BottomRight": (
                max(0, width - corner),
                max(0, height - corner),
                corner,
                corner,
            ),
        }
        for name, geometry in geometries.items():
            self.handles[name].setGeometry(*geometry)  # fluent-audit: allow resize hit targets
            self.handles[name].raise_()


class FluentTitleBar(QFrame):
    """One reusable title/command surface with an optional custom caption."""

    _INTERACTIVE_TYPES = (
        QAbstractButton,
        QLineEdit,
        QComboBox,
        QSpinBox,
        QAbstractSlider,
        QMenuBar,
    )

    def __init__(
        self,
        title: str = "",
        parent: QWidget | None = None,
        *,
        compact: bool = False,
        mode: TitleBarMode = TitleBarMode.NATIVE_FALLBACK,
    ) -> None:
        super().__init__(parent)
        self._compact = compact
        self._mode = mode
        self._bound_window: QWidget | None = None
        self._bound_handle: QWindow | None = None
        self._base_margins = QMargins(SPACE_S, 0, 0 if mode == TitleBarMode.FRAMELESS else SPACE_S, 0)
        self._command_widget: QWidget | None = None
        self._responsive_collapse_width = 640
        self._menu_attached = False
        self._system_menu: QMenu | None = None
        self._system_menu_shortcut: QShortcut | None = None

        set_fluent_property(self, "fluentRole", "titleBar")
        set_fluent_property(self, "fluentSize", "compact" if compact else "standard")
        set_fluent_property(self, "windowActive", True)
        self.setProperty("titleBarMode", mode.value)

        self._layout = QHBoxLayout(self)
        self._layout.setContentsMargins(
            self._base_margins.left(),
            self._base_margins.top(),
            self._base_margins.right(),
            self._base_margins.bottom(),
        )
        self._layout.setSpacing(SPACE_S_NUDGE)

        self._leading_host = QFrame(self)
        self._leading_host.setObjectName("titleBarLeadingHost")
        self._leading_layout = QHBoxLayout(self._leading_host)
        self._leading_layout.setContentsMargins(0, 0, 0, 0)
        self._leading_layout.setSpacing(SPACE_XS)

        self._menu_bar = QMenuBar(self)
        self._menu_bar.setObjectName("windowMenuBar")
        self._menu_bar.setNativeMenuBar(False)
        self._menu_bar.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Preferred)
        set_fluent_property(self._menu_bar, "fluentRole", "titleBarMenu")
        self._menu_bar.setProperty("titleBarInteractive", True)
        apply_accessible_identity(
            self._menu_bar,
            name="Application menu",
            identifier="shell.titlebar.menu",
        )

        self._title_label = QLabel(title, self)
        self._title_label.setObjectName("windowTitleLabel")
        set_fluent_property(self._title_label, "fluentTextRole", "caption1")
        self._title_label.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        self._title_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

        self._center_host = QFrame(self)
        self._center_host.setObjectName("titleBarCenterHost")
        self._center_layout = QHBoxLayout(self._center_host)
        self._center_layout.setContentsMargins(0, 0, 0, 0)
        self._center_layout.setSpacing(0)

        self._trailing_host = QFrame(self)
        self._trailing_host.setObjectName("titleBarTrailingHost")
        self._trailing_layout = QHBoxLayout(self._trailing_host)
        self._trailing_layout.setContentsMargins(0, 0, 0, 0)
        self._trailing_layout.setSpacing(SPACE_XXS)

        self._caption_host = QFrame(self)
        self._caption_host.setObjectName("windowCaptionControls")
        self._caption_layout = QHBoxLayout(self._caption_host)
        self._caption_layout.setContentsMargins(0, 0, 0, 0)
        self._caption_layout.setSpacing(0)
        self._caption_buttons: dict[str, QToolButton] = {}

        self._layout.addWidget(self._leading_host)
        self._layout.addWidget(self._title_label)
        self._layout.addStretch(1)
        self._layout.addWidget(self._center_host, 2)
        self._layout.addStretch(1)
        self._layout.addWidget(self._trailing_host)
        self._layout.addWidget(self._caption_host)

        if mode == TitleBarMode.FRAMELESS:
            self._install_caption_buttons()
        else:
            self._caption_host.hide()

        apply_accessible_identity(self, name="Window title and commands", identifier="shell.titlebar")

    @property
    def mode(self) -> TitleBarMode:
        return self._mode

    @property
    def menu_bar(self) -> QMenuBar:
        return self._menu_bar

    @property
    def caption_buttons(self) -> dict[str, QToolButton]:
        return dict(self._caption_buttons)

    def set_title(self, title: str) -> None:
        self._title_label.setText(title)
        if self._bound_window is not None:
            self._bound_window.setWindowTitle(title)

    def set_title_visible(self, visible: bool) -> None:
        """Hide only the visual label; the native accessible window title remains set."""
        self._title_label.setVisible(visible)

    def title(self) -> str:
        return self._title_label.text()

    def add_menu(self, title: str) -> QMenu:
        """Add an in-row application menu with normal Qt mnemonic behavior."""
        if not self._menu_attached:
            self._leading_layout.addWidget(self._menu_bar)
            self._menu_attached = True
        return self._menu_bar.addMenu(title)

    def add_leading_widget(self, widget: QWidget) -> None:
        widget.setProperty("titleBarInteractive", True)
        self._leading_layout.addWidget(widget)

    def add_trailing_widget(self, widget: QWidget) -> None:
        widget.setProperty("titleBarInteractive", True)
        self._trailing_layout.addWidget(widget)
        self._update_responsive_visibility()

    def set_command_widget(self, widget: QWidget | None) -> None:
        if self._command_widget is widget:
            return
        if self._command_widget is not None:
            self._center_layout.removeWidget(self._command_widget)
            self._command_widget.setParent(None)
        self._command_widget = widget
        if widget is not None:
            widget.setProperty("titleBarInteractive", True)
            self._center_layout.addWidget(widget)
        self._update_responsive_visibility()

    def set_responsive_collapse_width(self, width: int) -> None:
        """Set the width below which center and low-priority trailing content hides."""
        self._responsive_collapse_width = max(0, int(width))
        self._update_responsive_visibility()

    def _update_responsive_visibility(self) -> None:
        narrow = self.width() < self._responsive_collapse_width
        set_fluent_property(self, "titleBarNarrow", narrow)
        self._center_host.setVisible(not narrow and self._command_widget is not None)
        self._trailing_host.setVisible(not narrow and self._trailing_layout.count() > 0)

    def _install_caption_buttons(self) -> None:
        definitions = (
            ("minimize", "minimizeWindowButton", "Minimize window"),
            ("maximize", "maximizeRestoreWindowButton", "Maximize window"),
            ("close", "closeWindowButton", "Close window"),
        )
        for action, object_name, accessible_name in definitions:
            button = _CaptionButton(self._caption_host)
            button.setObjectName(object_name)
            button.setAutoRaise(True)
            button.setFocusPolicy(Qt.FocusPolicy.TabFocus)
            button.setProperty("titleBarAction", "caption")
            button.setProperty("captionAction", action)
            set_fluent_property(button, "fluentIconOnly", True)
            apply_accessible_identity(
                button,
                name=accessible_name,
                identifier=f"shell.titlebar.{action}",
            )
            button.setToolTip(accessible_name)
            self._caption_layout.addWidget(button)
            self._caption_buttons[action] = button

        self._caption_buttons["minimize"].clicked.connect(self._minimize_window)
        self._caption_buttons["maximize"].clicked.connect(self._toggle_maximize)
        self._caption_buttons["close"].clicked.connect(self._close_window)
        self.refresh_caption_icons()

    def refresh_caption_icons(self) -> None:
        """Refresh platform caption icons after palette, style, or state changes."""
        if not getattr(self, "_caption_buttons", None):
            return
        window = self._bound_window or self.window()
        maximized = bool(window and window.isMaximized())
        maximize_name = "Restore window" if maximized else "Maximize window"
        self._caption_buttons["minimize"].setIcon(self._caption_icon("minimize"))
        maximize_button = self._caption_buttons["maximize"]
        maximize_button.setIcon(self._caption_icon("restore" if maximized else "maximize"))
        maximize_button.setToolTip(maximize_name)
        maximize_button.setAccessibleName(maximize_name)
        self._caption_buttons["close"].setIcon(self._caption_icon("close"))

    def _caption_icon(self, name: str) -> QIcon:
        path = Path(__file__).with_name("icons") / f"{name}.svg"
        renderer = QSvgRenderer(QByteArray(path.read_bytes())) if path.is_file() else QSvgRenderer()
        if not renderer.isValid():
            fallback = {
                "minimize": QStyle.StandardPixmap.SP_TitleBarMinButton,
                "maximize": QStyle.StandardPixmap.SP_TitleBarMaxButton,
                "restore": QStyle.StandardPixmap.SP_TitleBarNormalButton,
                "close": QStyle.StandardPixmap.SP_TitleBarCloseButton,
            }
            return self.style().standardIcon(fallback[name])

        scale = 2
        pixmap = QPixmap(ICON_SIZE_SMALL * scale, ICON_SIZE_SMALL * scale)
        pixmap.setDevicePixelRatio(scale)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        renderer.render(painter, QRectF(0, 0, ICON_SIZE_SMALL, ICON_SIZE_SMALL))
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceIn)
        group = (
            QPalette.ColorGroup.Active
            if self._bound_window is None or self._bound_window.isActiveWindow()
            else QPalette.ColorGroup.Inactive
        )
        painter.fillRect(
            QRectF(0, 0, ICON_SIZE_SMALL, ICON_SIZE_SMALL),
            self.palette().color(group, QPalette.ColorRole.WindowText),
        )
        painter.end()
        return QIcon(pixmap)

    def bind_window(self, window: QWidget) -> None:
        """Bind activation, move, safe-area, caption, and system-menu behavior."""
        if self._bound_window is window:
            self._bind_window_handle()
            return
        if self._bound_window is not None:
            self._bound_window.removeEventFilter(self)
        self._bound_window = window
        window.installEventFilter(self)
        if self.title():
            window.setWindowTitle(self.title())
        self._update_active_state()
        self._bind_window_handle()
        if self._mode == TitleBarMode.FRAMELESS:
            self._ensure_system_menu()
            self.refresh_caption_icons()

    def _bind_window_handle(self) -> None:
        if self._bound_window is None:
            return
        handle = self._bound_window.windowHandle()
        if handle is None or handle is self._bound_handle:
            return
        self._bound_handle = handle
        safe_signal = getattr(handle, "safeAreaMarginsChanged", None)
        if safe_signal is not None:
            safe_signal.connect(self._on_safe_area_changed)
        self._update_safe_area()

    def _on_safe_area_changed(self, *_args: object) -> None:
        self._update_safe_area()

    def _update_safe_area(self) -> None:
        margins = QMargins()
        if self._mode == TitleBarMode.EXPANDED_CLIENT_AREA and self._bound_handle is not None:
            getter = getattr(self._bound_handle, "safeAreaMargins", None)
            if callable(getter):
                margins = getter()
        combined = QMargins(
            self._base_margins.left() + margins.left(),
            self._base_margins.top() + margins.top(),
            self._base_margins.right() + margins.right(),
            self._base_margins.bottom() + margins.bottom(),
        )
        self._layout.setContentsMargins(
            combined.left(), combined.top(), combined.right(), combined.bottom()
        )

    def _update_active_state(self) -> None:
        active = bool(self._bound_window and self._bound_window.isActiveWindow())
        set_fluent_property(self, "windowActive", active)
        self.refresh_caption_icons()

    def _minimize_window(self) -> None:
        window = self._bound_window or self.window()
        window.showMinimized()

    def _toggle_maximize(self) -> None:
        window = self._bound_window or self.window()
        if window.isMaximized():
            window.showNormal()
        else:
            window.showMaximized()
        self.refresh_caption_icons()

    def _close_window(self) -> None:
        window = self._bound_window or self.window()
        window.close()

    def _ensure_system_menu(self) -> None:
        if self._bound_window is None or self._system_menu is not None:
            return
        menu = QMenu(self._bound_window)
        menu.setObjectName("framelessSystemMenu")
        menu.setAccessibleName("Window menu")
        restore_action = menu.addAction("&Restore")
        restore_action.setObjectName("restoreWindowAction")
        restore_action.triggered.connect(self._restore_window)
        minimize_action = menu.addAction("Mi&nimize")
        minimize_action.setObjectName("minimizeWindowAction")
        minimize_action.triggered.connect(self._minimize_window)
        maximize_action = menu.addAction("Ma&ximize")
        maximize_action.setObjectName("maximizeWindowAction")
        maximize_action.triggered.connect(self._maximize_window)
        menu.addSeparator()
        close_action = menu.addAction("&Close")
        close_action.setObjectName("closeWindowAction")
        close_action.triggered.connect(self._close_window)
        menu.aboutToShow.connect(self._update_system_menu)
        self._system_menu = menu

        shortcut = QShortcut(QKeySequence("Alt+Space"), self._bound_window)
        shortcut.setObjectName("openWindowSystemMenuShortcut")
        shortcut.setContext(Qt.ShortcutContext.WindowShortcut)
        shortcut.activated.connect(self._show_system_menu)
        self._system_menu_shortcut = shortcut

    def _update_system_menu(self) -> None:
        if self._system_menu is None:
            return
        window = self._bound_window or self.window()
        restore = self._system_menu.findChild(QAction, "restoreWindowAction")
        minimize = self._system_menu.findChild(QAction, "minimizeWindowAction")
        maximize = self._system_menu.findChild(QAction, "maximizeWindowAction")
        if restore is not None:
            restore.setEnabled(window.isMaximized() or window.isMinimized())
        if minimize is not None:
            minimize.setEnabled(not window.isMinimized())
        if maximize is not None:
            maximize.setEnabled(not window.isMaximized())

    def _restore_window(self) -> None:
        (self._bound_window or self.window()).showNormal()

    def _maximize_window(self) -> None:
        (self._bound_window or self.window()).showMaximized()

    def _show_system_menu(self, global_position: QPoint | None = None) -> None:
        if self._system_menu is None:
            return
        if global_position is None:
            global_position = self.mapToGlobal(QPoint(0, self.height()))
        self._system_menu.popup(global_position)

    def eventFilter(self, watched: object, event: QEvent) -> bool:  # noqa: N802 - Qt API
        if watched is self._bound_window:
            if event.type() in (QEvent.Type.WindowActivate, QEvent.Type.WindowDeactivate):
                self._update_active_state()
            elif event.type() in (QEvent.Type.Show, QEvent.Type.WinIdChange):
                self._bind_window_handle()
            elif event.type() == QEvent.Type.WindowStateChange:
                self.refresh_caption_icons()
        return super().eventFilter(watched, event)

    def changeEvent(self, event: QEvent) -> None:  # noqa: N802 - Qt API
        if event.type() in (QEvent.Type.PaletteChange, QEvent.Type.StyleChange):
            self.refresh_caption_icons()
        super().changeEvent(event)

    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802 - Qt API
        super().showEvent(event)
        if self._bound_window is None:
            top = self.window()
            if top is not self:
                self.bind_window(top)
        self._bind_window_handle()

    def resizeEvent(self, event: QResizeEvent) -> None:  # noqa: N802 - Qt API
        super().resizeEvent(event)
        self._update_responsive_visibility()

    def _is_interactive(self, widget: QWidget | None) -> bool:
        current = widget
        while current is not None and current is not self:
            if bool(current.property("titleBarInteractive")):
                return True
            if isinstance(current, self._INTERACTIVE_TYPES):
                return True
            current = current.parentWidget()
        return False

    def _is_drag_position(self, position: QPoint) -> bool:
        return not self._is_interactive(self.childAt(position))

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt API
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._is_drag_position(event.position().toPoint())
        ):
            self._bind_window_handle()
            if self._bound_handle is not None:
                start_move = getattr(self._bound_handle, "startSystemMove", None)
                if callable(start_move) and start_move():
                    event.accept()
                    return
        super().mousePressEvent(event)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt API
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._is_drag_position(event.position().toPoint())
        ):
            self._toggle_maximize()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:  # noqa: N802 - Qt API
        if self._mode == TitleBarMode.FRAMELESS and self._is_drag_position(event.pos()):
            self._show_system_menu(event.globalPos())
            event.accept()
            return
        super().contextMenuEvent(event)
