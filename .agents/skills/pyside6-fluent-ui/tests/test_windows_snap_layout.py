"""Windows 11 Snap Layout and narrow title-row contracts."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "templates"))

try:
    from PySide6.QtCore import QPoint
    from PySide6.QtWidgets import QApplication, QLineEdit, QToolButton

    from pyside6_fluent_ui.title_bar import TitleBarMode
    from pyside6_fluent_ui.windows_title_bar import (
        HTMAXBUTTON,
        screen_point_from_lparam,
        windows_snap_layout_supported,
    )
    from pyside6_fluent_ui.workbench import FluentWorkbenchWindow
except ModuleNotFoundError:  # pragma: no cover - static-only skill validation environment
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 runtime is unavailable")
class WindowsSnapLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["windows-snap-layout-tests"])

    def test_screen_point_decoder_preserves_negative_virtual_coordinates(self) -> None:
        lparam = ((-20 & 0xFFFF) << 16) | (-10 & 0xFFFF)
        self.assertEqual(screen_point_from_lparam(lparam), QPoint(-10, -20))

    def test_adapter_owns_only_the_maximize_button_hit_region(self) -> None:
        window = FluentWorkbenchWindow(title_bar_mode=TitleBarMode.FRAMELESS)
        window.show()
        self.app.processEvents()

        adapter = window.windows_snap_layout_adapter
        self.assertIsNotNone(adapter)
        self.assertEqual(adapter.enabled, windows_snap_layout_supported())
        self.assertEqual(adapter.installed, windows_snap_layout_supported())

        maximize = window.title_bar.caption_buttons["maximize"]
        center = maximize.mapTo(window, maximize.rect().center())
        self.assertEqual(adapter.hit_test_client_position(center), HTMAXBUTTON)
        self.assertIsNone(adapter.hit_test_client_position(QPoint(10, window.height() // 2)))

        window.close()
        window.deleteLater()
        self.app.processEvents()

    def test_narrow_width_preserves_menus_and_caption_controls(self) -> None:
        window = FluentWorkbenchWindow(title_bar_mode=TitleBarMode.FRAMELESS)
        command = QLineEdit(window.title_bar)
        command.setObjectName("testCommandCenter")
        trailing = QToolButton(window.title_bar)
        trailing.setObjectName("testTrailingCommand")
        window.title_bar.set_command_widget(command)
        window.title_bar.add_trailing_widget(trailing)
        window.show()

        window.resize(500, 420)
        self.app.processEvents()
        self.assertLessEqual(window.minimumWidth(), 500)
        self.assertFalse(command.isVisible())
        self.assertFalse(trailing.isVisible())
        self.assertTrue(window.title_bar.menu_bar.isVisible())
        for button in window.title_bar.caption_buttons.values():
            self.assertTrue(button.isVisible())

        window.resize(800, 500)
        self.app.processEvents()
        self.assertTrue(command.isVisible())
        self.assertTrue(trailing.isVisible())

        window.close()
        window.deleteLater()
        self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
