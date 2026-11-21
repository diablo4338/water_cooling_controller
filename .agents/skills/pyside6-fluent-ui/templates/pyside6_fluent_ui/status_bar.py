"""Structured Fluent Status Bar with stable IDs, alignment, and priority."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Callable

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QToolButton, QWidget

from .metrics import ICON_SIZE_SMALL, SPACE_S_NUDGE, SPACE_XS
from .style import apply_accessible_identity, set_fluent_property


class StatusAlignment(str, Enum):
    LEFT = "left"
    RIGHT = "right"


class _PassiveStatusItem(QFrame):
    """Nonactionable icon/text status that does not masquerade as a button."""

    def __init__(
        self,
        text: str,
        icon: QIcon | None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        set_fluent_property(self, "fluentRole", "statusItemPassive")

        layout = QHBoxLayout(self)
        layout.setContentsMargins(SPACE_S_NUDGE, 0, SPACE_S_NUDGE, 0)
        layout.setSpacing(SPACE_XS)
        self._icon_label = QLabel(self)
        self._text_label = QLabel(text, self)
        self._icon_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self._text_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        layout.addWidget(self._icon_label)
        layout.addWidget(self._text_label)
        self.set_icon(icon)

    def set_text(self, text: str) -> None:
        self._text_label.setText(text)

    def set_icon(self, icon: QIcon | None) -> None:
        if icon is None or icon.isNull():
            self._icon_label.clear()
            self._icon_label.hide()
            return
        self._icon_label.setPixmap(icon.pixmap(ICON_SIZE_SMALL, ICON_SIZE_SMALL))
        self._icon_label.show()


@dataclass
class _StatusEntry:
    entry_id: str
    widget: QWidget
    alignment: StatusAlignment
    priority: int
    insertion_order: int
    action: Callable[[], None] | None
    severity: str


class FluentStatusBar(QFrame):
    """Compact global/contextual status surface."""

    entryActivated = Signal(str)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._entries: dict[str, _StatusEntry] = {}
        self._next_order = 0
        set_fluent_property(self, "fluentRole", "statusBar")
        set_fluent_property(self, "fluentSeverity", "normal")

        self._layout = QHBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._layout.setSpacing(0)
        self._left_layout = QHBoxLayout()
        self._left_layout.setContentsMargins(0, 0, 0, 0)
        self._left_layout.setSpacing(0)
        self._right_layout = QHBoxLayout()
        self._right_layout.setContentsMargins(0, 0, 0, 0)
        self._right_layout.setSpacing(0)
        self._layout.addLayout(self._left_layout)
        self._layout.addStretch(1)
        self._layout.addLayout(self._right_layout)

    def add_entry(
        self,
        entry_id: str,
        text: str,
        *,
        alignment: StatusAlignment | str = StatusAlignment.LEFT,
        priority: int = 0,
        icon: QIcon | None = None,
        tooltip: str | None = None,
        accessible_name: str | None = None,
        action: Callable[[], None] | None = None,
        severity: str = "normal",
    ) -> QWidget:
        if not entry_id or entry_id in self._entries:
            raise ValueError(f"Status entry ID must be non-empty and unique: {entry_id!r}")
        alignment = alignment if isinstance(alignment, StatusAlignment) else StatusAlignment(alignment)
        self._validate_severity(severity)

        if action is None:
            item: QWidget = _PassiveStatusItem(text, icon, self)
        else:
            button = QToolButton(self)
            button.setAutoRaise(True)
            button.setText(text)
            if icon is not None:
                button.setIcon(icon)
            button.setToolButtonStyle(
                Qt.ToolButtonStyle.ToolButtonTextBesideIcon
                if icon is not None
                else Qt.ToolButtonStyle.ToolButtonTextOnly
            )
            button.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
            button.clicked.connect(lambda _checked=False, key=entry_id: self._activate(key))
            set_fluent_property(button, "fluentRole", "statusItem")
            item = button

        set_fluent_property(item, "fluentSeverity", severity)
        if tooltip:
            item.setToolTip(tooltip)
        name = accessible_name or (tooltip if action and tooltip else text)
        apply_accessible_identity(
            item,
            name=name,
            description=tooltip,
            identifier=f"status.{entry_id}",
        )

        self._entries[entry_id] = _StatusEntry(
            entry_id=entry_id,
            widget=item,
            alignment=alignment,
            priority=priority,
            insertion_order=self._next_order,
            action=action,
            severity=severity,
        )
        self._next_order += 1
        self._rebuild()
        return item

    def update_entry(
        self,
        entry_id: str,
        *,
        text: str | None = None,
        tooltip: str | None = None,
        accessible_name: str | None = None,
        icon: QIcon | None = None,
        enabled: bool | None = None,
        severity: str | None = None,
        priority: int | None = None,
        visible: bool | None = None,
    ) -> None:
        entry = self._require(entry_id)
        widget = entry.widget
        if text is not None:
            if isinstance(widget, _PassiveStatusItem):
                widget.set_text(text)
            elif isinstance(widget, QToolButton):
                widget.setText(text)
        if tooltip is not None:
            widget.setToolTip(tooltip)
            widget.setAccessibleDescription(tooltip)
        if accessible_name is not None:
            widget.setAccessibleName(accessible_name)
        if icon is not None:
            if isinstance(widget, _PassiveStatusItem):
                widget.set_icon(icon)
            elif isinstance(widget, QToolButton):
                widget.setIcon(icon)
                widget.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        if enabled is not None:
            widget.setEnabled(enabled)
        if severity is not None:
            self._validate_severity(severity)
            entry.severity = severity
            set_fluent_property(widget, "fluentSeverity", severity)
        if priority is not None and priority != entry.priority:
            entry.priority = priority
            self._rebuild()
        if visible is not None:
            widget.setVisible(visible)

    def set_entry_visible(self, entry_id: str, visible: bool) -> None:
        self._require(entry_id).widget.setVisible(visible)

    def set_entry_enabled(self, entry_id: str, enabled: bool) -> None:
        self._require(entry_id).widget.setEnabled(enabled)

    def set_bar_severity(self, severity: str) -> None:
        self._validate_severity(severity)
        set_fluent_property(self, "fluentSeverity", severity)

    def entry_widget(self, entry_id: str) -> QWidget:
        return self._require(entry_id).widget

    def remove_entry(self, entry_id: str) -> None:
        entry = self._entries.pop(entry_id)
        self._left_layout.removeWidget(entry.widget)
        self._right_layout.removeWidget(entry.widget)
        entry.widget.deleteLater()

    def _activate(self, entry_id: str) -> None:
        entry = self._require(entry_id)
        if entry.action is not None and entry.widget.isEnabled():
            entry.action()
        self.entryActivated.emit(entry_id)

    @staticmethod
    def _clear_layout(layout: QHBoxLayout) -> None:
        while layout.count():
            item = layout.takeAt(0)
            del item

    def _rebuild(self) -> None:
        self._clear_layout(self._left_layout)
        self._clear_layout(self._right_layout)
        left = sorted(
            (entry for entry in self._entries.values() if entry.alignment == StatusAlignment.LEFT),
            key=lambda entry: (-entry.priority, entry.insertion_order),
        )
        # On the right, high priority is closest to the window edge (added last).
        right = sorted(
            (entry for entry in self._entries.values() if entry.alignment == StatusAlignment.RIGHT),
            key=lambda entry: (entry.priority, -entry.insertion_order),
        )
        for entry in left:
            self._left_layout.addWidget(entry.widget)
        for entry in right:
            self._right_layout.addWidget(entry.widget)

    @staticmethod
    def _validate_severity(severity: str) -> None:
        if severity not in {"normal", "info", "success", "warning", "danger"}:
            raise ValueError(f"Unknown status severity: {severity!r}")

    def _require(self, entry_id: str) -> _StatusEntry:
        try:
            return self._entries[entry_id]
        except KeyError as exc:
            raise KeyError(f"Unknown Status Bar entry: {entry_id!r}") from exc
