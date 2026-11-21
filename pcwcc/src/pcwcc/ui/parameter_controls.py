from collections.abc import Sequence
from typing import TypedDict

from PySide6.QtCore import QSignalBlocker, Qt, Signal
from PySide6.QtWidgets import QButtonGroup, QCheckBox, QHBoxLayout, QSlider, QSpinBox, QWidget


from ..gui_logic.constants import ParameterSpec


class ParameterChoices(QWidget):
    valueChanged = Signal(int)

    def __init__(self, label: str, choices: Sequence[tuple[int, str]]) -> None:
        super().__init__()
        self.setAccessibleName(label)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(16)
        self.buttons = QButtonGroup(self)
        for value, caption in choices:
            button = QCheckBox(caption)
            self.buttons.addButton(button, value)
            layout.addWidget(button)
        layout.addStretch()
        self.buttons.buttons()[0].setChecked(True)
        self.buttons.idToggled.connect(self._on_toggled)

    def _on_toggled(self, value: int, checked: bool) -> None:
        if checked:
            self.valueChanged.emit(value)

    def value(self) -> int:
        return self.buttons.checkedId()

    def setValue(self, value: int) -> None:
        button = self.buttons.button(int(value)) or self.buttons.buttons()[0]
        button.setChecked(True)


class ParameterNumber(QWidget):
    valueChanged = Signal(int)

    def __init__(self, label: str, minimum: int, maximum: int, slider_range: tuple[int, int]) -> None:
        super().__init__()
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        self.input = QSpinBox()
        self.input.setAccessibleName(label)
        self.input.setRange(minimum, maximum)
        self.input.setAlignment(Qt.AlignmentFlag.AlignRight)
        self.input.setKeyboardTracking(False)
        self.setFocusProxy(self.input)

        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setAccessibleName(label)
        self.slider.setRange(*slider_range)
        self.slider.setPageStep(max(1, (slider_range[1] - slider_range[0]) // 10))
        layout.addWidget(self.input)
        layout.addWidget(self.slider, 1)
        self.slider.valueChanged.connect(self.input.setValue)
        self.input.valueChanged.connect(self._sync_slider)

    def _sync_slider(self, value: int) -> None:
        # The numeric input also accepts stored values outside the slider's range.
        with QSignalBlocker(self.slider):
            self.slider.setValue(value)
        self.valueChanged.emit(value)

    def value(self) -> int:
        return self.input.value()

    def setValue(self, value: int) -> None:
        self.input.setValue(value)


class ParameterField(TypedDict):
    spec: ParameterSpec
    widget: QCheckBox | ParameterChoices | ParameterNumber
