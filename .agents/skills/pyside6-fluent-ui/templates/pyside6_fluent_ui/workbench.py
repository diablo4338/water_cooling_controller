"""Composable Fluent workbench shell for isolated validation and project adaptation."""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QAction, QIcon, QKeySequence, QShowEvent
from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QMainWindow,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from .activity_bar import FluentActivityBar
from .metrics import (
    SIDEBAR_MAXIMUM_WIDTH,
    SIDEBAR_MINIMUM_WIDTH,
    SIDEBAR_PREFERRED_WIDTH,
    WORKBENCH_MINIMUM_HEIGHT,
    WORKBENCH_SNAP_MINIMUM_WIDTH,
)
from .status_bar import FluentStatusBar
from .style import set_fluent_property
from .theme import FluentThemeManager
from .title_bar import (
    FluentTitleBar,
    FramelessWindowController,
    TitleBarMode,
    enable_expanded_client_area,
    enable_frameless_window,
)
from .window_frame import FluentWindowFrameController
from .windows_title_bar import WindowsSnapLayoutAdapter, WindowsSnapLayoutWindowMixin


@dataclass
class _WorkbenchView:
    view_id: str
    sidebar_index: int
    content_index: int
    has_sidebar: bool


class FluentWorkbenchWindow(WindowsSnapLayoutWindowMixin, QMainWindow):
    """Reference shell that routes stable Activity IDs to Sidebar/content stacks."""

    currentViewChanged = Signal(str)

    def __init__(
        self,
        title: str = "Fluent Workbench",
        parent: QWidget | None = None,
        *,
        compact: bool = False,
        title_bar_mode: TitleBarMode = TitleBarMode.NATIVE_FALLBACK,
        theme_manager: FluentThemeManager | None = None,
    ) -> None:
        super().__init__(parent)
        self._views: dict[str, _WorkbenchView] = {}
        self._compact = compact
        self.title_bar_mode = title_bar_mode

        if title_bar_mode == TitleBarMode.EXPANDED_CLIENT_AREA:
            if not enable_expanded_client_area(self):
                self.title_bar_mode = TitleBarMode.NATIVE_FALLBACK
        elif title_bar_mode == TitleBarMode.FRAMELESS:
            if not enable_frameless_window(self):
                self.title_bar_mode = TitleBarMode.NATIVE_FALLBACK

        self.setWindowTitle(title)
        self.resize(1200, 760)
        self.setMinimumSize(
            WORKBENCH_SNAP_MINIMUM_WIDTH,
            WORKBENCH_MINIMUM_HEIGHT,
        )

        self.window_frame = QFrame(self)
        self.window_frame.setObjectName("fluentWindowFrame")
        set_fluent_property(self.window_frame, "fluentRole", "windowFrame")
        frame_layout = QVBoxLayout(self.window_frame)
        frame_layout.setContentsMargins(0, 0, 0, 0)
        frame_layout.setSpacing(0)
        self.setCentralWidget(self.window_frame)

        central = QFrame(self.window_frame)
        central.setObjectName("fluentWorkbenchShell")
        set_fluent_property(central, "fluentRole", "workbench")
        root_layout = QVBoxLayout(central)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)
        frame_layout.addWidget(central)

        self.title_bar = FluentTitleBar(
            title,
            central,
            compact=compact,
            mode=self.title_bar_mode,
        )
        self._configure_window_menu()
        root_layout.addWidget(self.title_bar)

        body = QFrame(central)
        body_layout = QHBoxLayout(body)
        body_layout.setContentsMargins(0, 0, 0, 0)
        body_layout.setSpacing(0)
        root_layout.addWidget(body, 1)

        self.activity_bar = FluentActivityBar(body, compact=compact)
        self.activity_bar.currentChanged.connect(self.set_current_view)
        body_layout.addWidget(self.activity_bar)

        self.main_splitter = QSplitter(Qt.Orientation.Horizontal, body)
        self.main_splitter.setChildrenCollapsible(False)
        body_layout.addWidget(self.main_splitter, 1)

        self.sidebar_host = QFrame(self.main_splitter)
        set_fluent_property(self.sidebar_host, "fluentRole", "sidebar")
        sidebar_layout = QVBoxLayout(self.sidebar_host)
        sidebar_layout.setContentsMargins(0, 0, 0, 0)
        sidebar_layout.setSpacing(0)
        self.sidebar_stack = QStackedWidget(self.sidebar_host)
        sidebar_layout.addWidget(self.sidebar_stack)
        self.sidebar_host.setMinimumWidth(SIDEBAR_MINIMUM_WIDTH)
        self.sidebar_host.setMaximumWidth(SIDEBAR_MAXIMUM_WIDTH)
        self.sidebar_host.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Expanding)

        self.content_stack = QStackedWidget(self.main_splitter)
        set_fluent_property(self.content_stack, "fluentRole", "content")

        self.main_splitter.addWidget(self.sidebar_host)
        self.main_splitter.addWidget(self.content_stack)
        self.main_splitter.setStretchFactor(0, 0)
        self.main_splitter.setStretchFactor(1, 1)
        self.main_splitter.setSizes(
            [SIDEBAR_PREFERRED_WIDTH, max(1, self.width() - SIDEBAR_PREFERRED_WIDTH)]
        )

        self.status_bar = FluentStatusBar(central)
        root_layout.addWidget(self.status_bar)

        self.frameless_controller = (
            FramelessWindowController(self)
            if self.title_bar_mode == TitleBarMode.FRAMELESS
            else None
        )
        maximize_button = self.title_bar.caption_buttons.get("maximize")
        self.windows_snap_layout_adapter = (
            WindowsSnapLayoutAdapter(self, maximize_button)
            if self.title_bar_mode == TitleBarMode.FRAMELESS
            and maximize_button is not None
            else None
        )
        self.window_frame_controller = (
            FluentWindowFrameController(
                self,
                self.window_frame,
                theme_manager=theme_manager,
            )
            if self.title_bar_mode == TitleBarMode.FRAMELESS
            else None
        )

    def _configure_window_menu(self) -> None:
        """Install a minimal, functional menu in the reference shell."""
        file_menu = self.title_bar.add_menu("&File")
        file_menu.setObjectName("fileMenu")
        close_action = QAction("&Close Window", self)
        close_action.setObjectName("closeWindowAction")
        close_action.setShortcut(QKeySequence.StandardKey.Close)
        close_action.triggered.connect(self.close)
        file_menu.addAction(close_action)

        view_menu = self.title_bar.add_menu("&View")
        view_menu.setObjectName("viewMenu")
        full_screen_action = QAction("Toggle &Full Screen", self)
        full_screen_action.setObjectName("toggleFullScreenAction")
        full_screen_action.setShortcut(QKeySequence("F11"))
        full_screen_action.triggered.connect(self._toggle_full_screen)
        view_menu.addAction(full_screen_action)

    def _toggle_full_screen(self) -> None:
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()

    def add_view(
        self,
        view_id: str,
        label: str,
        icon: QIcon,
        content: QWidget,
        *,
        sidebar: QWidget | None = None,
        location: str = "primary",
        badge: str | int | None = None,
    ) -> None:
        if view_id in self._views:
            raise ValueError(f"Duplicate workbench view ID: {view_id!r}")
        sidebar_page = sidebar if sidebar is not None else QWidget(self.sidebar_stack)
        sidebar_index = self.sidebar_stack.addWidget(sidebar_page)
        content_index = self.content_stack.addWidget(content)
        self._views[view_id] = _WorkbenchView(
            view_id=view_id,
            sidebar_index=sidebar_index,
            content_index=content_index,
            has_sidebar=sidebar is not None,
        )
        self.activity_bar.add_item(
            view_id,
            label,
            icon,
            location="secondary" if location == "secondary" else "primary",
            badge=badge,
        )
        if len(self._views) == 1:
            self.set_current_view(view_id)

    def set_current_view(self, view_id: str) -> None:
        view = self._views.get(view_id)
        if view is None:
            raise KeyError(view_id)
        self.activity_bar.set_current(view_id)
        self.sidebar_stack.setCurrentIndex(view.sidebar_index)
        self.content_stack.setCurrentIndex(view.content_index)
        self.sidebar_host.setVisible(view.has_sidebar)
        if view.has_sidebar:
            sizes = self.main_splitter.sizes()
            if sizes and sizes[0] <= 0:
                self.main_splitter.setSizes([SIDEBAR_PREFERRED_WIDTH, max(1, sum(sizes))])
        self.currentViewChanged.emit(view_id)

    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802 - Qt API
        self.title_bar.bind_window(self)
        super().showEvent(event)
