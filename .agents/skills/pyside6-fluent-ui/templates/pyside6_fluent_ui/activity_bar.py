"""VS Code-inspired Activity Bar implemented with Qt-native buttons and routing IDs."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QIcon, QKeyEvent, QResizeEvent
from PySide6.QtWidgets import QButtonGroup, QFrame, QLabel, QToolButton, QVBoxLayout, QWidget

from .metrics import ICON_SIZE_LARGE, ICON_SIZE_SMALL, SPACE_XS
from .style import apply_accessible_identity, set_fluent_property


@dataclass
class _ActivityItem:
    item_id: str
    label: str
    button: "ActivityButton"
    location: Literal["primary", "secondary"]


class ActivityButton(QToolButton):
    """Checkable icon button with an optional compact badge."""

    def __init__(self, bar: "FluentActivityBar", item_id: str, label: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._bar = bar
        self.item_id = item_id
        self.label = label
        self._badge_value: str | None = None

        self.setCheckable(True)
        self.setAutoRaise(True)
        self.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonIconOnly)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        set_fluent_property(self, "fluentRole", "activityItem")
        self.setToolTip(label)
        apply_accessible_identity(self, name=label, identifier=f"activity.{item_id}")

        self._badge = QLabel(self)
        set_fluent_property(self._badge, "fluentRole", "activityBadge")
        self._badge.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self._badge.hide()

    def set_badge(self, value: str | int | None) -> None:
        """Set a short count/marker. Empty values hide the badge."""
        text = "" if value is None else str(value).strip()
        if text and text.isdigit() and int(text) > 99:
            text = "99+"
        self._badge_value = text or None
        self._badge.setText(text)
        self._badge.setVisible(bool(text))
        if text:
            self._badge.adjustSize()
            self._position_badge()
            self.setAccessibleDescription(f"{self.label}, {text}")
        else:
            self.setAccessibleDescription("")

    def _position_badge(self) -> None:
        if not self._badge.isVisible():
            return
        margin = SPACE_XS
        self._badge.adjustSize()
        x = max(margin, self.width() - self._badge.width() - margin)
        y = margin
        self._badge.move(x, y)  # fluent-audit: allow bounded badge overlay
        self._badge.raise_()

    def resizeEvent(self, event: QResizeEvent) -> None:  # noqa: N802 - Qt API
        super().resizeEvent(event)
        self._position_badge()

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 - Qt API
        if event.key() in (Qt.Key.Key_Up, Qt.Key.Key_Left):
            self._bar.focus_relative(self, -1)
            event.accept()
            return
        if event.key() in (Qt.Key.Key_Down, Qt.Key.Key_Right):
            self._bar.focus_relative(self, 1)
            event.accept()
            return
        if event.key() == Qt.Key.Key_Home:
            self._bar.focus_boundary(first=True)
            event.accept()
            return
        if event.key() == Qt.Key.Key_End:
            self._bar.focus_boundary(first=False)
            event.accept()
            return
        super().keyPressEvent(event)


class FluentActivityBar(QFrame):
    """Primary view-container switcher with stable string IDs."""

    currentChanged = Signal(str)

    def __init__(self, parent: QWidget | None = None, *, compact: bool = False) -> None:
        super().__init__(parent)
        self._compact = compact
        self._items: dict[str, _ActivityItem] = {}
        self._numeric_to_id: dict[int, str] = {}
        self._next_numeric_id = 0

        set_fluent_property(self, "fluentRole", "activityBar")
        set_fluent_property(self, "fluentSize", "compact" if compact else "standard")

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._layout.setSpacing(0)

        self._primary = QVBoxLayout()
        self._primary.setContentsMargins(0, 0, 0, 0)
        self._primary.setSpacing(0)
        self._secondary = QVBoxLayout()
        self._secondary.setContentsMargins(0, 0, 0, 0)
        self._secondary.setSpacing(0)

        self._layout.addLayout(self._primary)
        self._layout.addStretch(1)
        self._layout.addLayout(self._secondary)

        self._group = QButtonGroup(self)
        self._group.setExclusive(True)
        self._group.idClicked.connect(self._on_id_clicked)

    def add_item(
        self,
        item_id: str,
        label: str,
        icon: QIcon,
        *,
        location: Literal["primary", "secondary"] = "primary",
        badge: str | int | None = None,
        enabled: bool = True,
    ) -> ActivityButton:
        if not item_id or item_id in self._items:
            raise ValueError(f"Activity item ID must be non-empty and unique: {item_id!r}")
        if location not in {"primary", "secondary"}:
            raise ValueError(f"Unknown Activity Bar location: {location!r}")

        button = ActivityButton(self, item_id, label, self)
        button.setIcon(icon)
        icon_size = ICON_SIZE_SMALL if self._compact else ICON_SIZE_LARGE
        button.setIconSize(QSize(icon_size, icon_size))
        button.setEnabled(enabled)
        button.set_badge(badge)
        set_fluent_property(button, "fluentSize", "compact" if self._compact else "standard")

        numeric_id = self._next_numeric_id
        self._next_numeric_id += 1
        self._group.addButton(button, numeric_id)
        self._numeric_to_id[numeric_id] = item_id

        item = _ActivityItem(item_id=item_id, label=label, button=button, location=location)
        self._items[item_id] = item
        (self._primary if location == "primary" else self._secondary).addWidget(button)
        return button

    def _on_id_clicked(self, numeric_id: int) -> None:
        item_id = self._numeric_to_id.get(numeric_id)
        if item_id is not None:
            self.currentChanged.emit(item_id)

    def current_id(self) -> str | None:
        checked = self._group.checkedButton()
        return checked.item_id if isinstance(checked, ActivityButton) else None

    def set_current(self, item_id: str, *, emit: bool = False) -> None:
        item = self._items.get(item_id)
        if item is None:
            raise KeyError(item_id)
        if not item.button.isEnabled():
            raise ValueError(f"Activity item is disabled: {item_id!r}")
        if not item.button.isChecked():
            item.button.setChecked(True)
        if emit:
            self.currentChanged.emit(item_id)

    def set_badge(self, item_id: str, badge: str | int | None) -> None:
        self._items[item_id].button.set_badge(badge)

    def set_item_visible(self, item_id: str, visible: bool) -> None:
        self._items[item_id].button.setVisible(visible)

    def set_item_enabled(self, item_id: str, enabled: bool) -> None:
        self._items[item_id].button.setEnabled(enabled)

    def remove_item(self, item_id: str) -> None:
        item = self._items.pop(item_id)
        numeric_id = self._group.id(item.button)
        self._numeric_to_id.pop(numeric_id, None)
        self._group.removeButton(item.button)
        item.button.deleteLater()

    def focus_relative(self, source: ActivityButton, delta: int) -> None:
        buttons = [
            item.button
            for item in self._ordered_items()
            if item.button.isVisible() and item.button.isEnabled()
        ]
        if not buttons:
            return
        try:
            index = buttons.index(source)
        except ValueError:
            index = 0
        target = max(0, min(len(buttons) - 1, index + delta))
        buttons[target].setFocus(Qt.FocusReason.TabFocusReason)


    def focus_boundary(self, *, first: bool) -> None:
        buttons = [
            item.button
            for item in self._ordered_items()
            if item.button.isVisible() and item.button.isEnabled()
        ]
        if buttons:
            (buttons[0] if first else buttons[-1]).setFocus(Qt.FocusReason.TabFocusReason)

    def _ordered_items(self) -> list[_ActivityItem]:
        primary = [item for item in self._items.values() if item.location == "primary"]
        secondary = [item for item in self._items.values() if item.location == "secondary"]
        return primary + secondary
