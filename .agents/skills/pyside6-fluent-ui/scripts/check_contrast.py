#!/usr/bin/env python3
"""Check representative semantic foreground/background pairs using WCAG contrast."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

_HEX_RE = re.compile(r"^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")
_RGBA_RE = re.compile(r"^rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([01](?:\.\d+)?)\s*\)$")

TEXT_PAIRS = [
    ("text_primary", "canvas_background", 4.5),
    ("text_secondary", "canvas_background", 4.5),
    ("text_tertiary", "canvas_background", 4.5),
    ("text_on_brand", "brand_background", 4.5),
    ("selection_strong_foreground", "selection_strong_background", 4.5),
    ("success_foreground", "success_background", 4.5),
    ("warning_foreground", "warning_background", 4.5),
    ("danger_foreground", "danger_background", 4.5),
    ("info_foreground", "info_background", 4.5),
    ("shell_title_foreground_active", "shell_title_background_active", 4.5),
    ("shell_title_foreground_inactive", "shell_title_background_inactive", 4.5),
    ("shell_activity_foreground", "shell_activity_background", 4.5),
    ("shell_activity_foreground_inactive", "shell_activity_background", 3.0),
    ("shell_status_foreground", "shell_status_background", 4.5),
    ("shell_status_warning_foreground", "shell_status_warning_background", 4.5),
    ("shell_status_danger_foreground", "shell_status_danger_background", 4.5),
]

NON_TEXT_PAIRS = [
    ("border_accessible", "canvas_background", 3.0),
    ("focus_border", "canvas_background", 3.0),
    ("shell_activity_indicator", "shell_activity_background", 3.0),
    ("shell_status_item_focus", "shell_status_background", 3.0),
]


def srgb_channel(value: int) -> float:
    channel = value / 255.0
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def rgba(value: str) -> tuple[int, int, int, float]:
    text = value.strip()
    match = _HEX_RE.fullmatch(text)
    if match:
        raw = match.group(1)
        if len(raw) == 8:  # This package treats explicit 8-digit values as #RRGGBBAA.
            red, green, blue, alpha = (int(raw[i : i + 2], 16) for i in (0, 2, 4, 6))
            return red, green, blue, alpha / 255.0
        return int(raw[0:2], 16), int(raw[2:4], 16), int(raw[4:6], 16), 1.0
    match = _RGBA_RE.fullmatch(text)
    if match:
        return int(match.group(1)), int(match.group(2)), int(match.group(3)), float(match.group(4))
    raise ValueError(f"Unsupported color for contrast check: {value!r}")


def composite(
    foreground: tuple[int, int, int, float],
    background: tuple[int, int, int, float],
) -> tuple[int, int, int, float]:
    fr, fg, fb, fa = foreground
    br, bg, bb, ba = background
    out_alpha = fa + ba * (1 - fa)
    if out_alpha == 0:
        return 0, 0, 0, 0
    return (
        round((fr * fa + br * ba * (1 - fa)) / out_alpha),
        round((fg * fa + bg * ba * (1 - fa)) / out_alpha),
        round((fb * fa + bb * ba * (1 - fa)) / out_alpha),
        out_alpha,
    )


def luminance(color: tuple[int, int, int, float]) -> float:
    red, green, blue, _alpha = color
    return 0.2126 * srgb_channel(red) + 0.7152 * srgb_channel(green) + 0.0722 * srgb_channel(blue)


def contrast(foreground: str, background: str) -> float:
    bg = rgba(background)
    fg = composite(rgba(foreground), bg)
    lighter = max(luminance(fg), luminance(bg))
    darker = min(luminance(fg), luminance(bg))
    return (lighter + 0.05) / (darker + 0.05)


def load_values(root: Path, mode: str, profile: str) -> dict[str, str]:
    sys.path.insert(0, str(root / "templates"))
    from pyside6_fluent_ui.tokens import TokenRepository

    repository = TokenRepository.from_skill_root(root)
    return dict(repository.resolve(mode, shell_profile=profile).aliases)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()

    failures = 0
    for profile in ("fluent-workbench", "fluent-workbench-neutral-status"):
        for mode in ("light", "dark"):
            values = load_values(root, mode, profile)
            print(f"[{profile} / {mode}]")
            for foreground, background, target in TEXT_PAIRS + NON_TEXT_PAIRS:
                ratio = contrast(values[foreground], values[background])
                passed = ratio + 1e-9 >= target
                failures += 0 if passed else 1
                print(
                    f"{'PASS' if passed else 'FAIL'} {ratio:5.2f}:1 >= {target:.1f}:1  "
                    f"{foreground} on {background}"
                )
            print()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
