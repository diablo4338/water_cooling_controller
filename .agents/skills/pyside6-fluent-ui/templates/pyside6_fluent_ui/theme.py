"""Application theme manager for the reference PySide6 Fluent workbench scaffold."""

from __future__ import annotations

from enum import Enum
from pathlib import Path
import re
from tempfile import TemporaryDirectory

from PySide6.QtCore import QEvent, QObject, QPointF, Qt, Signal, QEasingCurve
from PySide6.QtGui import QColor, QFont, QFontDatabase, QPalette
from PySide6.QtWidgets import QApplication

from .style import render_qss_file
from .tokens import ResolvedTheme, TokenRepository, parse_cubic_bezier, parse_ms, parse_px


class ThemeMode(str, Enum):
    SYSTEM = "system"
    LIGHT = "light"
    DARK = "dark"
    HIGH_CONTRAST = "high-contrast"


class MotionMode(str, Enum):
    """Application-level motion preference.

    Qt does not expose a reliable cross-platform reduced-motion preference in all
    supported versions, so applications should persist this explicit setting.
    """

    FULL = "full"
    REDUCED = "reduced"
    NONE = "none"


_RGBA_RE = re.compile(
    r"^rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d*\.?\d+)\s*\)$"
)
_RGB_RE = re.compile(r"^rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)$")


def qcolor(value: str) -> QColor:
    """Parse Fluent hex/rgb/rgba/transparent values into QColor."""
    text = value.strip()
    if text == "transparent":
        return QColor(0, 0, 0, 0)
    match = _RGBA_RE.fullmatch(text)
    if match:
        red, green, blue = (int(match.group(i)) for i in range(1, 4))
        alpha_value = float(match.group(4))
        alpha = round(alpha_value * 255) if alpha_value <= 1 else round(alpha_value)
        return QColor(red, green, blue, max(0, min(255, alpha)))
    match = _RGB_RE.fullmatch(text)
    if match:
        return QColor(*(int(match.group(i)) for i in range(1, 4)))
    color = QColor(text)
    if not color.isValid():
        raise ValueError(f"Unsupported color value: {value!r}")
    return color


def qcolor_to_qss(color: QColor) -> str:
    """Serialize QColor without relying on QSS support for #AARRGGBB."""
    if color.alpha() >= 255:
        return color.name(QColor.NameFormat.HexRgb)
    return f"rgba({color.red()},{color.green()},{color.blue()},{color.alphaF():.4f})"  # fluent-audit: allow serializer


def _set_palette_color(
    palette: QPalette,
    role: QPalette.ColorRole,
    value: str,
    group: QPalette.ColorGroup | None = None,
) -> None:
    color = qcolor(value)
    if group is None:
        palette.setColor(role, color)
    else:
        palette.setColor(group, role, color)


def build_palette(theme: ResolvedTheme, base_palette: QPalette | None = None) -> QPalette:
    """Build a broad semantic QPalette; component visuals remain in QSS."""
    palette = QPalette(base_palette) if base_palette is not None else QPalette()
    a = theme.aliases
    role = QPalette.ColorRole
    group = QPalette.ColorGroup

    active_roles = {
        role.Window: a["window_background"],
        role.WindowText: a["text_primary"],
        role.Base: a["canvas_background"],
        role.AlternateBase: a["surface_secondary"],
        role.Text: a["text_primary"],
        role.Button: a["surface_background"],
        role.ButtonText: a["text_primary"],
        role.BrightText: a["danger_foreground"],
        role.Highlight: a["selection_strong_background"],
        role.HighlightedText: a["selection_strong_foreground"],
        role.Link: a["link"],
        role.LinkVisited: a["brand_foreground_secondary"],
        role.ToolTipBase: a["tooltip_background"],
        role.ToolTipText: a["tooltip_foreground"],
        role.Light: a["border_tertiary"],
        role.Midlight: a["border_subtle"],
        role.Mid: a["border_primary"],
        role.Dark: a["text_secondary"],
        role.Shadow: a["text_primary"],
    }
    placeholder_role = getattr(role, "PlaceholderText", None)
    if placeholder_role is not None:
        active_roles[placeholder_role] = a["text_tertiary"]
    accent_role = getattr(role, "Accent", None)
    if accent_role is not None:
        active_roles[accent_role] = a["brand_background"]

    for color_role, value in active_roles.items():
        _set_palette_color(palette, color_role, value, group.Active)
        _set_palette_color(palette, color_role, value, group.Inactive)

    disabled_roles = {
        role.Window: a["window_background"],
        role.WindowText: a["text_disabled"],
        role.Base: a["surface_disabled"],
        role.AlternateBase: a["surface_disabled"],
        role.Text: a["text_disabled"],
        role.Button: a["surface_disabled"],
        role.ButtonText: a["text_disabled"],
        role.Highlight: a["surface_disabled"],
        role.HighlightedText: a["text_disabled"],
        role.Link: a["text_disabled"],
        role.ToolTipBase: a["tooltip_background"],
        role.ToolTipText: a["tooltip_foreground"],
    }
    if placeholder_role is not None:
        disabled_roles[placeholder_role] = a["text_disabled"]
    if accent_role is not None:
        disabled_roles[accent_role] = a["text_disabled"]
    for color_role, value in disabled_roles.items():
        _set_palette_color(palette, color_role, value, group.Disabled)

    return palette


def high_contrast_alias_overrides(palette: QPalette) -> dict[str, str]:
    """Map key semantic aliases to the effective system palette."""
    role = QPalette.ColorRole
    group = QPalette.ColorGroup

    def active(color_role: QPalette.ColorRole) -> str:
        return qcolor_to_qss(palette.color(group.Active, color_role))

    def disabled(color_role: QPalette.ColorRole) -> str:
        return qcolor_to_qss(palette.color(group.Disabled, color_role))

    placeholder_role = getattr(role, "PlaceholderText", role.Text)
    overrides = {
        "window_background": active(role.Window),
        "canvas_background": active(role.Base),
        "surface_background": active(role.Button),
        "surface_background_hover": active(role.Button),
        "surface_background_pressed": active(role.Button),
        "surface_background_selected": active(role.Highlight),
        "surface_secondary": active(role.Window),
        "surface_tertiary": active(role.Base),
        "surface_elevated": active(role.Base),
        "surface_disabled": disabled(role.Button),
        "card_background": active(role.Base),
        "card_background_hover": active(role.Button),
        "card_background_pressed": active(role.Button),
        "card_background_selected": active(role.Highlight),
        "text_primary": active(role.WindowText),
        "text_secondary": active(role.Text),
        "text_tertiary": active(placeholder_role),
        "text_quaternary": active(placeholder_role),
        "text_disabled": disabled(role.Text),
        "text_on_brand": active(role.HighlightedText),
        "text_inverted": active(role.HighlightedText),
        "link": active(role.Link),
        "link_hover": active(role.Highlight),
        "link_pressed": active(role.Highlight),
        "border_primary": active(role.Mid),
        "border_hover": active(role.Highlight),
        "border_pressed": active(role.Highlight),
        "border_selected": active(role.Highlight),
        "border_subtle": active(role.Midlight),
        "border_tertiary": active(role.Midlight),
        "border_disabled": disabled(role.Mid),
        "border_accessible": active(role.WindowText),
        "focus_border": active(role.Highlight),
        "focus_inner": active(role.Base),
        "transparent": "transparent",
        "subtle_hover": active(role.Button),
        "subtle_pressed": active(role.Highlight),
        "subtle_selected": active(role.Highlight),
        "brand_background": active(role.Highlight),
        "brand_background_hover": active(role.Highlight),
        "brand_background_pressed": active(role.Highlight),
        "brand_background_selected": active(role.Highlight),
        "brand_background_subtle": active(role.Base),
        "brand_background_subtle_hover": active(role.Highlight),
        "brand_background_subtle_pressed": active(role.Highlight),
        "brand_foreground": active(role.Link),
        "brand_foreground_secondary": active(role.Link),
        "brand_border": active(role.Highlight),
        "brand_border_subtle": active(role.Mid),
        "selection_strong_background": active(role.Highlight),
        "selection_strong_foreground": active(role.HighlightedText),
        "selection_subtle_background": active(role.Highlight),
        "selection_subtle_foreground": active(role.HighlightedText),
        "overlay_background": "transparent",
        "scrollbar_overlay": active(role.WindowText),
        "success_background": active(role.Base),
        "success_background_strong": active(role.Highlight),
        "success_foreground": active(role.WindowText),
        "success_foreground_inverted": active(role.HighlightedText),
        "success_border": active(role.Highlight),
        "warning_background": active(role.Base),
        "warning_background_strong": active(role.Highlight),
        "warning_foreground": active(role.WindowText),
        "warning_foreground_inverted": active(role.HighlightedText),
        "warning_border": active(role.Highlight),
        "danger_background": active(role.Base),
        "danger_background_strong": active(role.Highlight),
        "danger_background_strong_hover": active(role.Highlight),
        "danger_background_strong_pressed": active(role.Highlight),
        "danger_foreground": active(role.WindowText),
        "danger_foreground_inverted": active(role.HighlightedText),
        "danger_border": active(role.Highlight),
        "info_background": active(role.Base),
        "info_background_strong": active(role.Highlight),
        "info_foreground": active(role.WindowText),
        "info_foreground_inverted": active(role.HighlightedText),
        "info_border": active(role.Highlight),
        "tooltip_background": active(role.ToolTipBase),
        "tooltip_foreground": active(role.ToolTipText),
        # Shell follows system highlight/window colors rather than fixed brand surfaces.
        "shell_workbench_background": active(role.Window),
        "shell_window_border_active": active(role.WindowText),
        "shell_window_border_inactive": active(role.Mid),
        "shell_sidebar_background": active(role.Window),
        "shell_sidebar_border": active(role.Mid),
        "shell_panel_background": active(role.Base),
        "shell_panel_border": active(role.Mid),
        "shell_title_background_active": active(role.Window),
        "shell_title_foreground_active": active(role.WindowText),
        "shell_title_background_inactive": active(role.Window),
        "shell_title_foreground_inactive": active(role.Text),
        "shell_title_border": active(role.Mid),
        "shell_title_command_background": active(role.Base),
        "shell_title_command_hover": active(role.Button),
        "shell_title_command_pressed": active(role.Button),
        "shell_title_command_focus": active(role.Highlight),
        "shell_activity_background": active(role.Window),
        "shell_activity_foreground": active(role.WindowText),
        "shell_activity_foreground_inactive": active(role.Text),
        "shell_activity_border": active(role.Mid),
        "shell_activity_indicator": active(role.Highlight),
        "shell_activity_indicator_focus": active(role.Highlight),
        "shell_activity_hover": active(role.Button),
        "shell_activity_pressed": active(role.Button),
        "shell_activity_badge_background": active(role.Highlight),
        "shell_activity_badge_foreground": active(role.HighlightedText),
        "shell_status_background": active(role.Highlight),
        "shell_status_foreground": active(role.HighlightedText),
        "shell_status_border": active(role.WindowText),
        "shell_status_item_hover": active(role.Highlight),
        "shell_status_item_pressed": active(role.Highlight),
        "shell_status_item_focus": active(role.HighlightedText),
        "shell_status_warning_background": active(role.Highlight),
        "shell_status_warning_foreground": active(role.HighlightedText),
        "shell_status_danger_background": active(role.Highlight),
        "shell_status_danger_hover": active(role.Highlight),
        "shell_status_danger_pressed": active(role.Highlight),
        "shell_status_danger_foreground": active(role.HighlightedText),
        "shell_status_progress_foreground": active(role.HighlightedText),
    }
    return overrides


def easing_curve_from_token(value: str) -> QEasingCurve:
    """Create a Qt Bézier easing curve from a Fluent cubic-bezier token."""
    x1, y1, x2, y2 = parse_cubic_bezier(value)
    curve = QEasingCurve(QEasingCurve.Type.BezierSpline)
    curve.addCubicBezierSegment(QPointF(x1, y1), QPointF(x2, y2), QPointF(1.0, 1.0))
    return curve


class FluentThemeManager(QObject):
    """Apply resolved Fluent aliases to a QApplication and follow system preferences."""

    themeChanged = Signal(object)
    motionChanged = Signal(str)

    def __init__(
        self,
        app: QApplication,
        repository: TokenRepository,
        *,
        qss_template: str | Path | None = None,
        mode: ThemeMode = ThemeMode.SYSTEM,
        motion_mode: MotionMode = MotionMode.FULL,
        shell_profile: str | None = "fluent-workbench",
        prefer_fluent_font: bool = True,
        apply_body_pixel_size: bool = False,
    ) -> None:
        super().__init__(app)
        self.app = app
        self.repository = repository
        self.mode = mode
        self.motion_mode = motion_mode
        self.shell_profile = shell_profile
        self.prefer_fluent_font = prefer_fluent_font
        self.apply_body_pixel_size = apply_body_pixel_size
        self.qss_template = Path(qss_template) if qss_template else Path(__file__).with_name("fluent.qss.in")
        self._qss_asset_cache = TemporaryDirectory(prefix="pyside6-fluent-ui-")
        self._current_theme: ResolvedTheme | None = None
        self._applying = False
        # Preserve the palette supplied by the platform/style before this manager
        # applies a custom theme. This is the source of truth for high-contrast
        # aliases and for color-scheme fallback on older Qt versions.
        self._system_palette = QPalette(self.app.palette())

        self.app.installEventFilter(self)
        self._connect_system_signals()

    @property
    def current_theme(self) -> ResolvedTheme | None:
        return self._current_theme

    def set_mode(self, mode: ThemeMode | str) -> None:
        new_mode = mode if isinstance(mode, ThemeMode) else ThemeMode(mode)
        if self.mode == new_mode:
            return
        self.mode = new_mode
        self.apply()

    def set_motion_mode(self, mode: MotionMode | str) -> None:
        new_mode = mode if isinstance(mode, MotionMode) else MotionMode(mode)
        if self.motion_mode == new_mode:
            return
        self.motion_mode = new_mode
        self.motionChanged.emit(new_mode.value)

    def animation_duration(self, alias: str = "duration_normal") -> int:
        """Return a motion-policy-adjusted duration in milliseconds."""
        theme = self._current_theme
        if theme is None:
            base_mode = "dark" if self._system_is_dark() else "light"
            theme = self.repository.resolve(base_mode, shell_profile=self.shell_profile)
        duration = parse_ms(theme.value(alias))
        if self.motion_mode == MotionMode.NONE:
            return 0
        if self.motion_mode == MotionMode.REDUCED:
            return min(duration, 100)
        return duration

    def set_shell_profile(self, profile: str | None) -> None:
        if self.shell_profile == profile:
            return
        self.shell_profile = profile
        self.apply()

    def _connect_system_signals(self) -> None:
        hints = self.app.styleHints()
        color_signal = getattr(hints, "colorSchemeChanged", None)
        if color_signal is not None:
            color_signal.connect(self._on_system_preference_changed)

        accessibility_getter = getattr(hints, "accessibility", None)
        if callable(accessibility_getter):
            accessibility = accessibility_getter()
            contrast_signal = getattr(accessibility, "contrastPreferenceChanged", None)
            if contrast_signal is not None:
                contrast_signal.connect(self._on_system_preference_changed)

    def _on_system_preference_changed(self, *_args: object) -> None:
        self._refresh_system_palette()
        if self.mode in {ThemeMode.SYSTEM, ThemeMode.HIGH_CONTRAST}:
            self.apply()

    def _refresh_system_palette(self) -> None:
        style = self.app.style()
        if style is not None:
            self._system_palette = QPalette(style.standardPalette())

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt API
        if watched is self.app and not self._applying:
            palette_change = getattr(QEvent.Type, "ApplicationPaletteChange", None)
            theme_change = getattr(QEvent.Type, "ThemeChange", None)
            event_type = event.type()
            if event_type in {value for value in (palette_change, theme_change) if value is not None}:
                # Our own palette changes are ignored by the `_applying` guard.
                # An external/system palette event updates the preserved source.
                self._refresh_system_palette()
                if self.mode in {ThemeMode.SYSTEM, ThemeMode.HIGH_CONTRAST}:
                    self.apply()
        return super().eventFilter(watched, event)

    def _system_is_dark(self) -> bool:
        hints = self.app.styleHints()
        scheme_getter = getattr(hints, "colorScheme", None)
        color_scheme_enum = getattr(Qt, "ColorScheme", None)
        if callable(scheme_getter) and color_scheme_enum is not None:
            scheme = scheme_getter()
            dark = getattr(color_scheme_enum, "Dark", None)
            light = getattr(color_scheme_enum, "Light", None)
            if dark is not None and scheme == dark:
                return True
            if light is not None and scheme == light:
                return False
        return self._system_palette.color(QPalette.ColorRole.Window).lightness() < 128

    def _system_high_contrast(self) -> bool:
        """Best-effort Qt 6.10+ contrast detection, guarded for older bindings."""
        hints = self.app.styleHints()
        accessibility_getter = getattr(hints, "accessibility", None)
        if not callable(accessibility_getter):
            return False
        accessibility = accessibility_getter()
        preference_getter = getattr(accessibility, "contrastPreference", None)
        if not callable(preference_getter):
            return False
        preference = preference_getter()
        name = getattr(preference, "name", str(preference))
        return "HighContrast" in name or name.endswith("High") or name == "High"

    def effective_mode(self) -> ThemeMode:
        if self.mode == ThemeMode.HIGH_CONTRAST:
            return ThemeMode.HIGH_CONTRAST
        if self.mode == ThemeMode.SYSTEM:
            if self._system_high_contrast():
                return ThemeMode.HIGH_CONTRAST
            return ThemeMode.DARK if self._system_is_dark() else ThemeMode.LIGHT
        return self.mode

    def _build_font(self, theme: ResolvedTheme) -> QFont:
        font = QFontDatabase.systemFont(QFontDatabase.SystemFont.GeneralFont)
        if self.prefer_fluent_font:
            families = set(QFontDatabase.families())
            for candidate in ("Segoe UI Variable", "Segoe UI"):
                if candidate in families:
                    font.setFamily(candidate)
                    break
        if self.apply_body_pixel_size:
            font.setPixelSize(int(round(parse_px(theme.aliases["font_body_size"]))))
        return font

    def apply(self) -> ResolvedTheme:
        """Resolve and apply the effective theme. Safe to call repeatedly."""
        if self._applying:
            return self._current_theme or self.repository.resolve("light", shell_profile=self.shell_profile)
        self._applying = True
        try:
            effective = self.effective_mode()
            use_dark_base = effective == ThemeMode.DARK or (
                effective == ThemeMode.HIGH_CONTRAST and self._system_is_dark()
            )
            base_name = "dark" if use_dark_base else "light"
            theme = self.repository.resolve(base_name, shell_profile=self.shell_profile)
            if effective == ThemeMode.HIGH_CONTRAST:
                theme = theme.with_alias_overrides(
                    high_contrast_alias_overrides(self._system_palette),
                    name="high-contrast",
                )

            palette = build_palette(theme, self._system_palette if effective == ThemeMode.HIGH_CONTRAST else None)
            qss = render_qss_file(
                self.qss_template,
                theme,
                asset_directory=self._qss_asset_cache.name,
            )
            self.app.setFont(self._build_font(theme))
            self.app.setPalette(palette)
            self.app.setStyleSheet(qss)  # fluent-audit: allow generated application QSS
            self._current_theme = theme
            self.themeChanged.emit(theme)
            return theme
        finally:
            self._applying = False
