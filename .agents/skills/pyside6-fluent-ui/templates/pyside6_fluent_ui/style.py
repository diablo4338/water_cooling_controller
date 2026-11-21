"""QSS rendering, semantic properties, and accessibility helpers."""

from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import re
from tempfile import TemporaryDirectory
from typing import Mapping

from PySide6.QtCore import QObject
from PySide6.QtWidgets import QWidget

from .tokens import ResolvedTheme, TokenValidationError

_PLACEHOLDER_RE = re.compile(r"@\{([A-Za-z_][A-Za-z0-9_]*)\}")
_RGBA_COLOR_RE = re.compile(
    r"^rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d*\.?\d+)\s*\)$"
)
_RGB_COLOR_RE = re.compile(r"^rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)$")

_SYMBOLIC_QSS_ASSETS: dict[str, tuple[str, str]] = {
    "spin_up_icon": ("chevron-up.svg", "text_secondary"),
    "spin_down_icon": ("chevron-down.svg", "text_secondary"),
    "spin_up_icon_interactive": ("chevron-up.svg", "text_primary"),
    "spin_down_icon_interactive": ("chevron-down.svg", "text_primary"),
    "spin_up_icon_disabled": ("chevron-up.svg", "text_disabled"),
    "spin_down_icon_disabled": ("chevron-down.svg", "text_disabled"),
}
_FALLBACK_QSS_ASSET_CACHE: TemporaryDirectory | None = None


def _fallback_qss_asset_directory() -> Path:
    """Keep generated symbolic assets alive for direct renderer consumers."""
    global _FALLBACK_QSS_ASSET_CACHE
    if _FALLBACK_QSS_ASSET_CACHE is None:
        _FALLBACK_QSS_ASSET_CACHE = TemporaryDirectory(prefix="pyside6-fluent-ui-")
    return Path(_FALLBACK_QSS_ASSET_CACHE.name)


def _svg_color(value: str) -> tuple[str, str | None]:
    """Convert Qt/QSS functional colors to SVG-safe color and opacity values."""
    text = value.strip()
    if text == "transparent":
        return "#000000", "0"  # fluent-audit: allow fully transparent serializer base

    rgba = _RGBA_COLOR_RE.fullmatch(text)
    if rgba:
        red, green, blue = (max(0, min(255, int(rgba.group(i)))) for i in range(1, 4))
        alpha_value = float(rgba.group(4))
        alpha = alpha_value if alpha_value <= 1 else alpha_value / 255
        opacity = f"{max(0.0, min(1.0, alpha)):.4f}".rstrip("0").rstrip(".")
        return f"#{red:02x}{green:02x}{blue:02x}", opacity

    rgb = _RGB_COLOR_RE.fullmatch(text)
    if rgb:
        red, green, blue = (max(0, min(255, int(rgb.group(i)))) for i in range(1, 4))
        return f"#{red:02x}{green:02x}{blue:02x}", None

    return text, None


def _render_symbolic_qss_assets(
    requested: set[str],
    theme: ResolvedTheme,
    output_directory: Path,
) -> dict[str, str]:
    """Tint SVG templates from semantic aliases and return QSS-safe paths."""
    output_directory.mkdir(parents=True, exist_ok=True)
    icon_root = Path(__file__).with_name("icons")
    values: dict[str, str] = {}

    for placeholder in sorted(requested):
        filename, color_alias = _SYMBOLIC_QSS_ASSETS[placeholder]
        source_path = icon_root / filename
        try:
            source = source_path.read_text(encoding="utf-8")
        except FileNotFoundError as exc:
            raise TokenValidationError(f"Symbolic QSS asset not found: {source_path}") from exc
        if "currentColor" not in source:
            raise TokenValidationError(
                f"Symbolic QSS asset must use currentColor: {source_path}"
            )

        color, opacity = _svg_color(theme.value(color_alias))
        rendered = source.replace("currentColor", color)
        if opacity is not None and opacity != "1":
            rendered = rendered.replace(
                f'fill="{color}"',
                f'fill="{color}" fill-opacity="{opacity}"',
            ).replace(
                f'stroke="{color}"',
                f'stroke="{color}" stroke-opacity="{opacity}"',
            )
        digest = sha256(rendered.encode("utf-8")).hexdigest()[:12]
        target = output_directory / f"{source_path.stem}-{color_alias}-{digest}.svg"
        if not target.exists():
            target.write_text(rendered, encoding="utf-8")
        values[placeholder] = target.resolve().as_posix()

    return values


def render_qss(template: str, values: Mapping[str, str]) -> str:
    """Render semantic `@{name}` placeholders and fail on unknown values."""
    missing: set[str] = set()

    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in values:
            missing.add(name)
            return match.group(0)
        return str(values[name])

    rendered = _PLACEHOLDER_RE.sub(replace, template)
    if missing:
        raise TokenValidationError(f"QSS references unknown aliases: {', '.join(sorted(missing))}")
    leftovers = sorted(set(_PLACEHOLDER_RE.findall(rendered)))
    if leftovers:
        raise TokenValidationError(f"Unresolved QSS aliases: {', '.join(leftovers)}")
    return rendered


def render_qss_file(
    path: str | Path,
    theme: ResolvedTheme,
    *,
    asset_directory: str | Path | None = None,
) -> str:
    """Render semantic aliases and theme-tinted symbolic SVG asset paths."""
    template = Path(path).read_text(encoding="utf-8")
    placeholders = set(_PLACEHOLDER_RE.findall(template))
    requested_assets = placeholders & set(_SYMBOLIC_QSS_ASSETS)
    values = dict(theme.aliases)
    if requested_assets:
        output_directory = (
            Path(asset_directory)
            if asset_directory is not None
            else _fallback_qss_asset_directory()
        )
        values.update(
            _render_symbolic_qss_assets(requested_assets, theme, output_directory)
        )
    return render_qss(template, values)


def repolish(widget: QWidget) -> None:
    """Refresh one widget after a dynamic-property change."""
    style = widget.style()
    if style is not None:
        style.unpolish(widget)
        style.polish(widget)
    widget.update()


def set_fluent_property(widget: QWidget, name: str, value: object) -> None:
    """Set a semantic dynamic property and repolish only when it changed."""
    if widget.property(name) == value:
        return
    widget.setProperty(name, value)
    repolish(widget)


def apply_accessible_identity(
    obj: QObject,
    *,
    name: str,
    description: str | None = None,
    identifier: str | None = None,
) -> None:
    """Apply accessible identity with feature detection for newer Qt APIs."""
    set_name = getattr(obj, "setAccessibleName", None)
    if callable(set_name):
        set_name(name)
    if description:
        set_description = getattr(obj, "setAccessibleDescription", None)
        if callable(set_description):
            set_description(description)
    if identifier:
        set_identifier = getattr(obj, "setAccessibleIdentifier", None)
        if callable(set_identifier):
            set_identifier(identifier)
