"""Small reusable Fluent composites used by the gallery scaffold."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QStyle, QToolButton, QVBoxLayout, QWidget

from .metrics import SPACE_L, SPACE_M, SPACE_S, SPACE_XXS
from .style import apply_accessible_identity, set_fluent_property


class FluentCard(QFrame):
    """Simple semantic card; callers own its content and behavior."""

    def __init__(self, parent: QWidget | None = None, *, padding: int = SPACE_L, spacing: int = SPACE_M) -> None:
        super().__init__(parent)
        set_fluent_property(self, "fluentRole", "card")
        self.content_layout = QVBoxLayout(self)
        self.content_layout.setContentsMargins(padding, padding, padding, padding)
        self.content_layout.setSpacing(spacing)


class FluentMessageBar(QFrame):
    """Inline informational/status surface with optional dismiss action."""

    def __init__(
        self,
        message: str,
        parent: QWidget | None = None,
        *,
        severity: str = "info",
        title: str | None = None,
        dismissible: bool = False,
    ) -> None:
        super().__init__(parent)
        self.set_severity(severity)
        set_fluent_property(self, "fluentRole", "messageBar")
        apply_accessible_identity(self, name=title or severity.capitalize(), description=message)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(SPACE_M, SPACE_S, SPACE_S, SPACE_S)
        layout.setSpacing(SPACE_S)

        text_layout = QVBoxLayout()
        text_layout.setContentsMargins(0, 0, 0, 0)
        text_layout.setSpacing(SPACE_XXS)
        if title:
            title_label = QLabel(title, self)
            set_fluent_property(title_label, "fluentTextRole", "bodyStrong")
            text_layout.addWidget(title_label)
        message_label = QLabel(message, self)
        message_label.setWordWrap(True)
        set_fluent_property(message_label, "fluentTextRole", "body")
        text_layout.addWidget(message_label)
        layout.addLayout(text_layout, 1)

        if dismissible:
            dismiss = QToolButton(self)
            dismiss.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_TitleBarCloseButton))
            dismiss.setToolTip("Dismiss")
            dismiss.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
            set_fluent_property(dismiss, "fluentAppearance", "subtle")
            set_fluent_property(dismiss, "fluentIconOnly", True)
            apply_accessible_identity(dismiss, name="Dismiss message")
            dismiss.clicked.connect(self.hide)
            layout.addWidget(dismiss)

    def set_severity(self, severity: str) -> None:
        if severity not in {"info", "success", "warning", "danger"}:
            raise ValueError(f"Unsupported severity: {severity!r}")
        set_fluent_property(self, "fluentSeverity", severity)
