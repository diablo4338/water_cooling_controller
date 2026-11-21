from __future__ import annotations

import os
from pathlib import Path
import re
import sys
from tempfile import TemporaryDirectory
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "templates"
if str(TEMPLATES) not in sys.path:
    sys.path.insert(0, str(TEMPLATES))

from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QSpinBox, QStyle, QStyleOptionSpinBox

from pyside6_fluent_ui.style import render_qss_file, set_fluent_property
from pyside6_fluent_ui.theme import FluentThemeManager, ThemeMode
from pyside6_fluent_ui.tokens import TokenRepository


class SpinBoxStyleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])
        cls.repository = TokenRepository.from_skill_root(ROOT)
        cls.manager = FluentThemeManager(
            cls.app,
            cls.repository,
            mode=ThemeMode.LIGHT,
        )
        cls.manager.apply()

    @staticmethod
    def _subcontrol_rect(spin: QSpinBox, subcontrol: QStyle.SubControl):
        option = QStyleOptionSpinBox()
        spin.initStyleOption(option)
        return spin.style().subControlRect(
            QStyle.ComplexControl.CC_SpinBox,
            option,
            subcontrol,
            spin,
        )

    def _shown_spin(self) -> QSpinBox:
        spin = QSpinBox()
        spin.setRange(0, 10)
        spin.setValue(5)
        spin.resize(180, spin.sizeHint().height())
        spin.show()
        self.app.processEvents()
        self.addCleanup(spin.close)
        return spin

    def test_standard_geometry_and_native_interaction_are_preserved(self) -> None:
        spin = self._shown_spin()
        up = self._subcontrol_rect(spin, QStyle.SubControl.SC_SpinBoxUp)
        down = self._subcontrol_rect(spin, QStyle.SubControl.SC_SpinBoxDown)

        self.assertEqual(up.width(), 24)
        self.assertEqual(down.width(), 24)
        self.assertEqual(up.height(), 16)
        self.assertEqual(down.height(), 16)
        self.assertEqual(up.left(), down.left())
        self.assertLess(up.right(), spin.rect().right())
        self.assertLess(down.right(), spin.rect().right())

        QTest.mouseClick(spin, Qt.MouseButton.LeftButton, pos=up.center())
        self.assertEqual(spin.value(), 6)
        QTest.mouseClick(spin, Qt.MouseButton.LeftButton, pos=down.center())
        self.assertEqual(spin.value(), 5)
        QTest.keyClick(spin, Qt.Key.Key_Up)
        self.assertEqual(spin.value(), 6)

    def test_compact_density_uses_twelve_pixel_stepper_rows(self) -> None:
        spin = self._shown_spin()
        standard_height = self._subcontrol_rect(
            spin, QStyle.SubControl.SC_SpinBoxUp
        ).height()
        set_fluent_property(spin, "fluentSize", "compact")
        self.app.processEvents()
        compact_height = self._subcontrol_rect(
            spin, QStyle.SubControl.SC_SpinBoxUp
        ).height()

        self.assertEqual(standard_height, 16)
        self.assertEqual(compact_height, 12)
        self.assertLess(spin.sizeHint().height(), 29)

    def test_read_only_and_disabled_controls_do_not_step(self) -> None:
        spin = self._shown_spin()
        up = self._subcontrol_rect(spin, QStyle.SubControl.SC_SpinBoxUp)
        spin.setReadOnly(True)
        QTest.mouseClick(spin, Qt.MouseButton.LeftButton, pos=up.center())
        self.assertEqual(spin.value(), 5)

        spin.setReadOnly(False)
        spin.setEnabled(False)
        QTest.mouseClick(spin, Qt.MouseButton.LeftButton, pos=up.center())
        self.assertEqual(spin.value(), 5)

    def test_symbolic_assets_are_tinted_from_each_theme(self) -> None:
        qss_path = ROOT / "templates/pyside6_fluent_ui/fluent.qss.in"
        source_up = qss_path.with_name("icons") / "chevron-up.svg"
        self.assertIn("currentColor", source_up.read_text(encoding="utf-8"))

        with TemporaryDirectory() as directory:
            rendered_by_mode: dict[str, str] = {}
            for mode in ("light", "dark"):
                theme = self.repository.resolve(mode)
                rendered = render_qss_file(
                    qss_path,
                    theme,
                    asset_directory=Path(directory) / mode,
                )
                self.assertNotRegex(rendered, r"@\{[A-Za-z_][A-Za-z0-9_]*\}")
                paths = [Path(value) for value in re.findall(r'image: url\("([^"]+)"\)', rendered)]
                self.assertGreaterEqual(len(paths), 6)
                self.assertTrue(all(path.is_file() for path in paths))
                secondary = next(path for path in paths if "text_secondary" in path.name)
                icon = secondary.read_text(encoding="utf-8")
                self.assertIn(theme.value("text_secondary"), icon)
                self.assertNotIn("currentColor", icon)
                rendered_by_mode[mode] = secondary.name

            self.assertNotEqual(rendered_by_mode["light"], rendered_by_mode["dark"])

    def test_rgba_semantic_colors_render_as_svg_hex_and_opacity(self) -> None:
        qss_path = ROOT / "templates/pyside6_fluent_ui/fluent.qss.in"
        theme = self.repository.resolve("light").with_alias_overrides(
            {
                "text_primary": "rgba(0,0,0,0.8941)",
                "text_secondary": "rgba(0,0,0,0.8941)",
            },
            name="high-contrast-test",
        )
        with TemporaryDirectory() as directory:
            rendered = render_qss_file(
                qss_path,
                theme,
                asset_directory=directory,
            )
            paths = [Path(value) for value in re.findall(r'image: url\("([^"]+)"\)', rendered)]
            active = next(path for path in paths if "text_secondary" in path.name)
            icon = active.read_text(encoding="utf-8")
            self.assertIn('fill="#000000"', icon)
            self.assertIn('fill-opacity="0.8941"', icon)
            self.assertNotIn("rgba(", icon)

    def test_qss_owns_spin_button_states_and_frame_geometry(self) -> None:
        qss = (ROOT / "templates/pyside6_fluent_ui/fluent.qss.in").read_text(
            encoding="utf-8"
        )
        start = qss.index("/* SpinButton:")
        end = qss.index("QComboBox::drop-down", start)
        spin_qss = qss[start:end]

        for required in (
            "subcontrol-origin: padding",
            "::up-button:hover",
            "::down-button:pressed",
            "::up-button:disabled",
            "::down-button:off",
            "[readOnly=\"true\"]::up-button",
            "fluentInvalid=\"true\"]:focus",
            "fluentSize=\"compact\"",
            "@{spin_up_icon}",
            "@{spin_down_icon_disabled}",
        ):
            self.assertIn(required, spin_qss)
        self.assertNotIn(":read-only::", spin_qss)
        self.assertNotRegex(spin_qss, r"#[0-9A-Fa-f]{3,8}\b")


if __name__ == "__main__":
    unittest.main()
