"""Runtime contracts for the workbench title-bar profiles."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "templates"))

try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication, QMenuBar, QToolButton

    from pyside6_fluent_ui.title_bar import TitleBarMode
    from pyside6_fluent_ui.workbench import FluentWorkbenchWindow
except ModuleNotFoundError:  # pragma: no cover - static-only skill validation environment
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 runtime is unavailable")
class TitleBarTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["title-bar-tests"])

    def test_native_fallback_retains_native_window_frame(self) -> None:
        window = FluentWorkbenchWindow(title_bar_mode=TitleBarMode.NATIVE_FALLBACK)
        self.assertFalse(window.windowFlags() & Qt.WindowType.FramelessWindowHint)
        self.assertEqual(window.title_bar.caption_buttons, {})
        self.assertIsInstance(window.findChild(QMenuBar, "windowMenuBar"), QMenuBar)
        window.close()
        window.deleteLater()

    def test_frameless_profile_has_one_row_caption_contract(self) -> None:
        window = FluentWorkbenchWindow(title_bar_mode=TitleBarMode.FRAMELESS)
        window.show()
        self.app.processEvents()
        self.assertTrue(window.windowFlags() & Qt.WindowType.FramelessWindowHint)
        self.assertLessEqual(window.title_bar.height(), 40)
        self.assertEqual(len(window.frameless_controller.handles), 8)
        self.assertEqual(set(window.title_bar.caption_buttons), {"minimize", "maximize", "close"})
        for button in window.title_bar.caption_buttons.values():
            self.assertIsInstance(button, QToolButton)
            self.assertTrue(button.objectName())
            self.assertTrue(button.accessibleName())
            self.assertTrue(button.toolTip())
        self.assertEqual(
            [action.text().replace("&", "") for action in window.title_bar.menu_bar.actions()],
            ["File", "View"],
        )
        window.close()
        window.deleteLater()


if __name__ == "__main__":
    unittest.main()
