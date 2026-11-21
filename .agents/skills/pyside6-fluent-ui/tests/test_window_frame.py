"""Runtime contracts for the frameless workbench window boundary."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "templates"))

try:
    from PySide6.QtGui import QColor
    from PySide6.QtWidgets import QApplication

    from pyside6_fluent_ui.theme import FluentThemeManager, ThemeMode, qcolor
    from pyside6_fluent_ui.title_bar import TitleBarMode
    from pyside6_fluent_ui.tokens import TokenRepository
    from pyside6_fluent_ui.window_frame import qcolor_to_colorref
    from pyside6_fluent_ui.workbench import FluentWorkbenchWindow
except ModuleNotFoundError:  # pragma: no cover - static-only skill validation environment
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 runtime is unavailable")
class WindowFrameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["window-frame-tests"])
        cls.manager = FluentThemeManager(
            cls.app,
            TokenRepository.from_skill_root(ROOT),
            mode=ThemeMode.LIGHT,
        )
        cls.manager.apply()

    def test_colorref_uses_win32_byte_order(self) -> None:
        self.assertEqual(qcolor_to_colorref(QColor("#123456")), 0x563412)

    def test_frameless_shell_exposes_stable_boundary_state(self) -> None:
        window = FluentWorkbenchWindow(
            title_bar_mode=TitleBarMode.FRAMELESS,
            theme_manager=self.manager,
        )
        window.show()
        self.app.processEvents()

        self.assertEqual(window.window_frame.objectName(), "fluentWindowFrame")
        self.assertEqual(window.window_frame.property("fluentRole"), "windowFrame")
        self.assertIn(window.window_frame_controller.mode, {"native", "client"})
        self.assertIsInstance(window.window_frame.property("windowActive"), bool)
        self.assertTrue(window.window_frame.property("windowFrameVisible"))
        if window.window_frame_controller.mode == "client":
            image = window.grab().toImage()
            edge = image.pixelColor(0, image.height() // 2)
            expected = (
                window.window_frame_controller.active_border_color
                if window.window_frame.property("windowActive")
                else window.window_frame_controller.inactive_border_color
            )
            self.assertEqual(edge, expected)

        window.showMaximized()
        self.app.processEvents()
        self.assertFalse(window.window_frame.property("windowFrameVisible"))

        window.close()
        window.deleteLater()
        self.app.processEvents()

    def test_theme_changes_update_native_border_colors(self) -> None:
        window = FluentWorkbenchWindow(
            title_bar_mode=TitleBarMode.FRAMELESS,
            theme_manager=self.manager,
        )
        window.show()
        for mode in (ThemeMode.DARK, ThemeMode.HIGH_CONTRAST):
            self.manager.set_mode(mode)
            self.app.processEvents()
            theme = self.manager.current_theme
            self.assertIsNotNone(theme)
            self.assertEqual(
                window.window_frame_controller.active_border_color,
                qcolor(theme.aliases["shell_window_border_active"]),
            )
            self.assertEqual(
                window.window_frame_controller.inactive_border_color,
                qcolor(theme.aliases["shell_window_border_inactive"]),
            )
        self.manager.set_mode(ThemeMode.LIGHT)
        window.close()
        window.deleteLater()
        self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
