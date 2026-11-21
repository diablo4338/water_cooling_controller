"""Height transitions for inline feedback rows."""

from PySide6.QtCore import QEasingCurve, QVariantAnimation
from PySide6.QtGui import QResizeEvent
from PySide6.QtWidgets import QLabel, QSizePolicy, QWidget

from .theme import FluentThemeManager


class InlineMessage(QWidget):
    def __init__(self, label: QLabel) -> None:
        super().__init__()
        self.label = label
        label.setParent(self)
        label.hide()
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setFixedHeight(0)
        self._target_height = 0
        self._animation = QVariantAnimation(self)
        self._animation.setDuration(160)
        self._animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._animation.valueChanged.connect(self.setFixedHeight)

    def set_message(self, text: str) -> None:
        if text == self.label.text() and self.label.isHidden() == (not text):
            return
        self.label.setText(text)
        self.label.setVisible(bool(text))
        self._refresh_height()

    def resizeEvent(self, event: QResizeEvent) -> None:
        super().resizeEvent(event)
        if event.size().width() != event.oldSize().width():
            self._refresh_height()

    def _refresh_height(self) -> None:
        height = 0
        if not self.label.isHidden():
            height = self.label.heightForWidth(self.width())
            if height < 0:
                height = self.label.sizeHint().height()
            self.label.setGeometry(0, 0, self.width(), height)
        if height == self._target_height:
            return

        self._target_height = height
        self._animation.stop()
        if not self.isVisible() or FluentThemeManager.for_app().reduced_motion:
            self.setFixedHeight(height)
        else:
            self._animation.setStartValue(self.height())
            self._animation.setEndValue(height)
            self._animation.start()
