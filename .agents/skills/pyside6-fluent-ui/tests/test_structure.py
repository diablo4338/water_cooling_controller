"""Guardrails for the generic skill package."""

from __future__ import annotations

import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StructureTests(unittest.TestCase):
    def test_manifest_confirms_no_user_repository_inspection(self) -> None:
        manifest = json.loads((ROOT / "resources/source-manifest.json").read_text(encoding="utf-8"))
        self.assertFalse(
            manifest["based_on_phase1"]["scope"]["repository_inspection_performed"]
        )

    def test_skill_forbids_framework_substitution(self) -> None:
        skill = (ROOT / "SKILL.md").read_text(encoding="utf-8")
        self.assertIn("Do not introduce React, WinUI, XAML, or browser dependencies", skill)
        self.assertIn("Choose the title profile from the requested outcome", skill)

    def test_single_row_title_bar_contract_is_bundled(self) -> None:
        title_bar = (
            ROOT / "templates/pyside6_fluent_ui/title_bar.py"
        ).read_text(encoding="utf-8")
        qss = (
            ROOT / "templates/pyside6_fluent_ui/fluent.qss.in"
        ).read_text(encoding="utf-8")
        self.assertIn("class FramelessWindowController", title_bar)
        self.assertIn("def add_menu", title_bar)
        self.assertNotIn("NotImplementedError", title_bar)
        self.assertIn('QMenuBar[fluentRole="titleBarMenu"]', qss)
        self.assertIn(
            'QMenuBar[fluentRole="titleBarMenu"]::item:selected:focus',
            qss,
        )
        self.assertNotIn(
            'QMenuBar[fluentRole="titleBarMenu"]::item:focus {',
            qss,
        )
        self.assertIn('QToolButton[titleBarAction="caption"]', qss)

    def test_frameless_window_boundary_contract_is_bundled(self) -> None:
        package = ROOT / "templates/pyside6_fluent_ui"
        frame = (package / "window_frame.py").read_text(encoding="utf-8")
        qss = (package / "fluent.qss.in").read_text(encoding="utf-8")
        self.assertIn("class FluentWindowFrameController", frame)
        self.assertIn("DWMWA_BORDER_COLOR", frame)
        self.assertIn('QFrame[fluentRole="windowFrame"]', qss)
        self.assertIn("@{shell_window_border_active}", qss)
        self.assertIn("@{shell_window_border_inactive}", qss)

    def test_single_canonical_implementation_set(self) -> None:
        package = ROOT / "templates/pyside6_fluent_ui"
        obsolete = [
            "fluent_tokens.py",
            "fluent_theme.py",
            "fluent_palette.py",
            "fluent_activity_bar.py",
            "fluent_status_bar.py",
            "fluent_title_bar.py",
        ]
        self.assertFalse([name for name in obsolete if (package / name).exists()])

    def test_spin_box_contract_and_symbolic_icons_are_bundled(self) -> None:
        package = ROOT / "templates/pyside6_fluent_ui"
        qss = (package / "fluent.qss.in").read_text(encoding="utf-8")
        state_matrix = json.loads(
            (ROOT / "resources/component-state-matrix.json").read_text(encoding="utf-8")
        )
        self.assertIn("spin_box", state_matrix["components"])
        self.assertIn("subcontrol-origin: padding", qss)
        self.assertIn("@{spin_up_icon}", qss)
        self.assertIn("@{spin_down_icon_disabled}", qss)
        for name in ("chevron-up.svg", "chevron-down.svg"):
            icon = (package / "icons" / name).read_text(encoding="utf-8")
            self.assertIn("currentColor", icon)


if __name__ == "__main__":
    unittest.main()
