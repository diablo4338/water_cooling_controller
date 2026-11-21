"""Fluent appearance for the desktop interface."""

from __future__ import annotations

from string import Template

from PySide6.QtCore import QObject, QSettings, Qt, Signal
from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import QApplication, QWidget

from .control_style import FluentControlStyle
from .styles import STYLES
from .tokens import COLORS


def set_visual(widget: QWidget, name: str, value: str) -> None:
    if widget.property(name) != value:
        widget.setProperty(name, value)
        style = widget.style()
        style.unpolish(widget)
        style.polish(widget)
        widget.update()


def describe(widget: QWidget, text: str) -> None:
    widget.setToolTip(text)
    widget.setAccessibleDescription(text)


def theme_color(alias: str) -> QColor:
    manager = FluentThemeManager.for_app()
    if manager.colors:
        return QColor(manager.colors[alias])
    role = {
        "canvas_background": QPalette.ColorRole.Base,
        "border_primary": QPalette.ColorRole.Mid,
        "border_subtle": QPalette.ColorRole.Midlight,
        "brand_foreground": QPalette.ColorRole.Link,
        "success_background": QPalette.ColorRole.AlternateBase,
    }.get(alias, QPalette.ColorRole.Text)
    return manager.app.palette().color(role)


class FluentThemeManager(QObject):
    theme_changed = Signal()

    @classmethod
    def for_app(cls) -> FluentThemeManager:
        app = QApplication.instance()
        if not isinstance(app, QApplication):
            raise RuntimeError("A QApplication is required for the desktop theme")
        manager = app.findChild(cls, "_pcwcc_theme")
        if manager is None:
            manager = cls(app)
            manager.setObjectName("_pcwcc_theme")
        return manager

    def __init__(self, app: QApplication) -> None:
        super().__init__(app)
        self.app = app
        self.settings = QSettings("PCWCC", "Desktop")
        self.reduced_motion = self.settings.value("reduced_motion", False, type=bool)
        saved_mode = self.settings.value("appearance", "system")
        self.mode = (
            saved_mode if isinstance(saved_mode, str) and saved_mode in ("system", "light", "dark", "contrast")
            else "system"
        )
        self.platform_style = app.style().objectName()
        self.colors: dict[str, str] = {}
        hints = app.styleHints()
        hints.colorSchemeChanged.connect(self.apply)
        self.accessibility = hints.accessibility() if hasattr(hints, "accessibility") else None
        contrast_changed = getattr(self.accessibility, "contrastPreferenceChanged", None)
        if contrast_changed is not None:
            contrast_changed.connect(self.apply)
        self.apply()

    def set_reduced_motion(self, enabled: bool) -> None:
        self.reduced_motion = enabled
        self.settings.setValue("reduced_motion", enabled)

    def set_mode(self, mode: str) -> None:
        self.mode = mode
        self.settings.setValue("appearance", mode)
        self.apply()

    def apply(self, *_args: object) -> None:
        preference = getattr(self.accessibility, "contrastPreference", lambda: None)()
        high_contrast = getattr(preference, "name", "") in ("High", "HighContrast")
        if self.mode == "contrast" or high_contrast:
            self.colors = {}
            self.app.setStyleSheet("")
            self.app.setStyle(self.platform_style)
            self.app.setPalette(QPalette())
        else:
            dark = self.mode == "dark" or (
                self.mode == "system" and self.app.styleHints().colorScheme() == Qt.ColorScheme.Dark
            )
            self.colors = {name: shades[dark] for name, shades in COLORS.items()}
            self.app.setStyle(FluentControlStyle(self.colors))
            self.app.setPalette(self._palette())
            self.app.setStyleSheet(Template(STYLES).substitute(self.colors))
        self.theme_changed.emit()

    def _palette(self) -> QPalette:
        palette = self.app.style().standardPalette()
        role = QPalette.ColorRole
        for key, alias in {
            role.Window: "window_background",
            role.Base: "canvas_background",
            role.AlternateBase: "surface_tertiary",
            role.Button: "canvas_background",
            role.Text: "text_primary",
            role.WindowText: "text_primary",
            role.ButtonText: "text_primary",
            role.PlaceholderText: "text_secondary",
            role.Highlight: "brand_background",
            role.HighlightedText: "text_on_brand",
            role.Link: "brand_foreground",
            role.ToolTipBase: "tooltip_background",
            role.ToolTipText: "tooltip_foreground",
        }.items():
            palette.setColor(key, QColor(self.colors[alias]))
        for key in (role.Text, role.WindowText, role.ButtonText, role.PlaceholderText):
            palette.setColor(QPalette.ColorGroup.Disabled, key, QColor(self.colors["text_disabled"]))
        return palette
