#!/usr/bin/env python3
"""Verify the live Win32 maximize hit-test and activation contract."""

from __future__ import annotations

import ctypes
from pathlib import Path
import sys


SKILL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SKILL_ROOT / "templates"))

from PySide6.QtWidgets import QApplication  # noqa: E402

from pyside6_fluent_ui.title_bar import TitleBarMode  # noqa: E402
from pyside6_fluent_ui.windows_title_bar import (  # noqa: E402
    HTMAXBUTTON,
    WM_NCHITTEST,
    WM_NCLBUTTONDOWN,
    WM_NCLBUTTONUP,
    windows_snap_layout_supported,
)
from pyside6_fluent_ui.workbench import FluentWorkbenchWindow  # noqa: E402


def main() -> int:
    if not windows_snap_layout_supported():
        print("SKIP: Windows 11 or newer is required")
        return 0

    app = QApplication.instance() or QApplication(["verify-windows-snap-layout"])
    window = FluentWorkbenchWindow(title_bar_mode=TitleBarMode.FRAMELESS)
    window.resize(900, 600)
    window.show()
    app.processEvents()

    adapter = window.windows_snap_layout_adapter
    if adapter is None or not adapter.installed:
        raise RuntimeError("Snap Layout adapter is not attached to the frameless window")

    hwnd = ctypes.c_void_p(int(window.winId()))
    user32 = ctypes.windll.user32
    user32.GetDpiForWindow.argtypes = [ctypes.c_void_p]
    user32.GetDpiForWindow.restype = ctypes.c_uint
    user32.SendMessageW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
    ]
    user32.SendMessageW.restype = ctypes.c_ssize_t

    maximize = window.title_bar.caption_buttons["maximize"]
    logical = maximize.mapToGlobal(maximize.rect().center())
    scale = max(1.0, user32.GetDpiForWindow(hwnd) / 96.0)
    physical_x = round(logical.x() * scale)
    physical_y = round(logical.y() * scale)
    lparam = ((physical_y & 0xFFFF) << 16) | (physical_x & 0xFFFF)

    result = int(user32.SendMessageW(hwnd, WM_NCHITTEST, 0, lparam))
    if result != HTMAXBUTTON:
        raise AssertionError(f"Expected HTMAXBUTTON ({HTMAXBUTTON}), got {result}")

    user32.SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTMAXBUTTON, lparam)
    user32.SendMessageW(hwnd, WM_NCLBUTTONUP, HTMAXBUTTON, lparam)
    app.processEvents()
    app.processEvents()
    if not window.isMaximized():
        raise AssertionError("Native maximize-button activation did not maximize the window")

    window.close()
    app.processEvents()
    print("PASS: HTMAXBUTTON routing and native maximize activation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
