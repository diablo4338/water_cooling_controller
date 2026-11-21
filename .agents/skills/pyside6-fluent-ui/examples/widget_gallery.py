#!/usr/bin/env python3
"""Isolated gallery for the PySide6 Fluent UI skill.

Run from the skill root:
    python examples/widget_gallery.py
    python examples/widget_gallery.py --native-titlebar
    python examples/widget_gallery.py --theme dark --expanded-titlebar
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

SKILL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SKILL_ROOT / "templates"))

from PySide6.QtCore import Qt  # noqa: E402
from PySide6.QtWidgets import (  # noqa: E402
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QRadioButton,
    QScrollArea,
    QSpinBox,
    QStyle,
    QTableWidget,
    QTableWidgetItem,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from pyside6_fluent_ui.metrics import (  # noqa: E402
    SPACE_L,
    SPACE_M,
    SPACE_S,
    SPACE_XXL,
    SPACE_XXXL,
)
from pyside6_fluent_ui.status_bar import StatusAlignment  # noqa: E402
from pyside6_fluent_ui.style import apply_accessible_identity, set_fluent_property  # noqa: E402
from pyside6_fluent_ui.theme import (  # noqa: E402
    FluentThemeManager,
    MotionMode,
    ThemeMode,
)
from pyside6_fluent_ui.title_bar import TitleBarMode  # noqa: E402
from pyside6_fluent_ui.tokens import TokenRepository  # noqa: E402
from pyside6_fluent_ui.widgets import FluentCard, FluentMessageBar  # noqa: E402
from pyside6_fluent_ui.workbench import FluentWorkbenchWindow  # noqa: E402

def section_label(text: str) -> QLabel:
    label = QLabel(text)
    set_fluent_property(label, "fluentTextRole", "sectionTitle")
    return label


def make_sidebar(title: str, items: list[str]) -> QWidget:
    frame = QFrame()
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(SPACE_M, SPACE_L, SPACE_M, SPACE_M)
    layout.setSpacing(SPACE_S)
    heading = QLabel(title)
    set_fluent_property(heading, "fluentTextRole", "subtitle")
    layout.addWidget(heading)
    navigation = QListWidget()
    navigation.addItems(items)
    if items:
        navigation.setCurrentRow(0)
    layout.addWidget(navigation, 1)
    return frame


def button(text: str, appearance: str, *, enabled: bool = True) -> QPushButton:
    control = QPushButton(text)
    control.setEnabled(enabled)
    set_fluent_property(control, "fluentAppearance", appearance)
    return control


def component_page(*, compact: bool = False) -> QWidget:
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setFrameShape(QFrame.Shape.NoFrame)
    page = QWidget()
    set_fluent_property(page, "fluentSize", "compact" if compact else "standard")
    layout = QVBoxLayout(page)
    layout.setContentsMargins(SPACE_XXL, SPACE_XXL, SPACE_XXL, SPACE_XXXL)
    layout.setSpacing(SPACE_XXL)

    title = QLabel("Fluent component gallery")
    set_fluent_property(title, "fluentTextRole", "pageTitle")
    layout.addWidget(title)
    subtitle = QLabel("Standard Qt controls styled through semantic tokens and dynamic properties.")
    set_fluent_property(subtitle, "fluentTextRole", "body")
    layout.addWidget(subtitle)

    actions = FluentCard()
    actions.content_layout.addWidget(section_label("Actions"))
    row = QHBoxLayout()
    row.setSpacing(SPACE_S)
    for label, appearance in (
        ("Primary", "primary"),
        ("Secondary", "secondary"),
        ("Outline", "outline"),
        ("Subtle", "subtle"),
        ("Transparent", "transparent"),
        ("Delete", "danger"),
    ):
        row.addWidget(button(label, appearance))
    row.addWidget(button("Disabled", "secondary", enabled=False))
    row.addStretch(1)
    actions.content_layout.addLayout(row)
    layout.addWidget(actions)

    fields = FluentCard()
    fields.content_layout.addWidget(section_label("Inputs and states"))
    form = QFormLayout()
    form.setHorizontalSpacing(16)
    form.setVerticalSpacing(12)
    name = QLineEdit("Ada Lovelace")
    form.addRow("Name", name)
    search = QLineEdit()
    search.setPlaceholderText("Search…")
    form.addRow("Search", search)
    invalid = QLineEdit("not-an-email")
    set_fluent_property(invalid, "fluentInvalid", True)
    invalid.setAccessibleDescription("Enter a valid email address")
    form.addRow("Email (invalid)", invalid)
    readonly = QLineEdit("Read-only value")
    readonly.setReadOnly(True)
    form.addRow("Identifier", readonly)
    combo = QComboBox()
    combo.addItems(["Standard density", "Compact density", "Comfortable density"])
    form.addRow("Density", combo)
    spin = QSpinBox()
    spin.setObjectName("galleryOpacitySpin")
    spin.setAccessibleName("Opacity")
    spin.setRange(0, 100)
    spin.setValue(42)
    spin.setSuffix(" %")
    form.addRow("Opacity", spin)
    double_spin = QDoubleSpinBox()
    double_spin.setObjectName("galleryInterfaceScaleSpin")
    double_spin.setAccessibleName("Interface scale")
    double_spin.setRange(0.5, 3.0)
    double_spin.setSingleStep(0.05)
    double_spin.setValue(1.25)
    double_spin.setSuffix(" x")
    form.addRow("Interface scale", double_spin)
    compact_spin = QSpinBox()
    compact_spin.setObjectName("galleryCompactSpin")
    compact_spin.setAccessibleName("Compact stepper")
    compact_spin.setRange(0, 20)
    compact_spin.setValue(8)
    set_fluent_property(compact_spin, "fluentSize", "compact")
    form.addRow("Compact stepper", compact_spin)
    invalid_spin = QSpinBox()
    invalid_spin.setObjectName("galleryInvalidSpin")
    invalid_spin.setAccessibleName("Invalid iteration count")
    invalid_spin.setAccessibleDescription("Iteration count must be at least one")
    invalid_spin.setRange(0, 100)
    set_fluent_property(invalid_spin, "fluentInvalid", True)
    form.addRow("Iterations (invalid)", invalid_spin)
    readonly_spin = QSpinBox()
    readonly_spin.setObjectName("galleryReadOnlySpin")
    readonly_spin.setAccessibleName("Read-only revision")
    readonly_spin.setValue(12)
    readonly_spin.setReadOnly(True)
    form.addRow("Revision (read-only)", readonly_spin)
    disabled_spin = QSpinBox()
    disabled_spin.setObjectName("galleryDisabledSpin")
    disabled_spin.setAccessibleName("Disabled retry count")
    disabled_spin.setValue(3)
    disabled_spin.setEnabled(False)
    form.addRow("Retries (disabled)", disabled_spin)
    notes = QPlainTextEdit("Multiline plain text uses QPlainTextEdit.")
    notes.setMaximumHeight(90)
    form.addRow("Notes", notes)
    fields.content_layout.addLayout(form)

    checks = QHBoxLayout()
    checks.addWidget(QCheckBox("Enabled option"))
    mixed = QCheckBox("Mixed selection")
    mixed.setTristate(True)
    mixed.setCheckState(Qt.CheckState.PartiallyChecked)
    checks.addWidget(mixed)
    radio_a = QRadioButton("Choice A")
    radio_b = QRadioButton("Choice B")
    radio_a.setChecked(True)
    checks.addWidget(radio_a)
    checks.addWidget(radio_b)
    checks.addStretch(1)
    fields.content_layout.addLayout(checks)
    layout.addWidget(fields)

    feedback = FluentCard()
    feedback.content_layout.addWidget(section_label("Feedback"))
    feedback.content_layout.addWidget(FluentMessageBar("An informational message belongs in the page layout."))
    feedback.content_layout.addWidget(
        FluentMessageBar("The operation completed successfully.", severity="success", title="Saved")
    )
    feedback.content_layout.addWidget(
        FluentMessageBar("Review these settings before continuing.", severity="warning", title="Warning")
    )
    feedback.content_layout.addWidget(
        FluentMessageBar("The selected item could not be removed.", severity="danger", title="Error", dismissible=True)
    )
    progress = QProgressBar()
    progress.setRange(0, 100)
    progress.setValue(64)
    feedback.content_layout.addWidget(progress)
    layout.addWidget(feedback)

    data_card = FluentCard()
    data_card.content_layout.addWidget(section_label("Data view"))
    table = QTableWidget(5, 3)
    table.setHorizontalHeaderLabels(["Item", "Status", "Owner"])
    data = [
        ("Theme tokens", "Complete", "Design"),
        ("Keyboard review", "Review", "Accessibility"),
        ("Widget states", "Complete", "UI team"),
        ("Build 042", "Processing", "CI"),
        ("Release notes", "Blocked", "Docs"),
    ]
    for row, values in enumerate(data):
        for column, value in enumerate(values):
            table.setItem(row, column, QTableWidgetItem(value))
    table.setAlternatingRowColors(True)
    table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
    table.setMinimumHeight(220)
    table.horizontalHeader().setStretchLastSection(True)
    data_card.content_layout.addWidget(table)
    layout.addWidget(data_card)
    layout.addStretch(1)

    scroll.setWidget(page)
    return scroll


def search_page() -> QWidget:
    page = QWidget()
    layout = QVBoxLayout(page)
    layout.setContentsMargins(SPACE_XXL, SPACE_XXL, SPACE_XXL, SPACE_XXL)
    layout.setSpacing(SPACE_L)
    title = QLabel("Search workspace")
    set_fluent_property(title, "fluentTextRole", "pageTitle")
    layout.addWidget(title)
    field = QLineEdit()
    field.setPlaceholderText("Search all project data")
    layout.addWidget(field)
    layout.addWidget(FluentMessageBar("Type a query to populate results."))
    layout.addStretch(1)
    return page


def settings_page(theme_manager: FluentThemeManager) -> QWidget:
    page = QWidget()
    layout = QVBoxLayout(page)
    layout.setContentsMargins(SPACE_XXL, SPACE_XXL, SPACE_XXL, SPACE_XXL)
    layout.setSpacing(SPACE_L)
    title = QLabel("Appearance settings")
    set_fluent_property(title, "fluentTextRole", "pageTitle")
    layout.addWidget(title)

    card = FluentCard()
    form = QFormLayout()
    mode = QComboBox()
    for item in ThemeMode:
        mode.addItem(item.value, item)
    mode.setCurrentText(theme_manager.mode.value)
    mode.currentIndexChanged.connect(lambda index: theme_manager.set_mode(mode.itemData(index)))
    form.addRow("Theme", mode)
    motion = QComboBox()
    for item in MotionMode:
        motion.addItem(item.value, item)
    motion.setCurrentText(theme_manager.motion_mode.value)
    motion.currentIndexChanged.connect(
        lambda index: theme_manager.set_motion_mode(motion.itemData(index))
    )
    form.addRow("Motion", motion)
    card.content_layout.addLayout(form)
    card.content_layout.addWidget(QCheckBox("Follow system contrast preference"))
    layout.addWidget(card)
    layout.addStretch(1)
    return page


def build_window(app: QApplication, args: argparse.Namespace) -> tuple[FluentWorkbenchWindow, FluentThemeManager]:
    repository = TokenRepository.from_skill_root(SKILL_ROOT)
    manager = FluentThemeManager(
        app,
        repository,
        mode=ThemeMode(args.theme),
        shell_profile=args.shell_profile,
    )
    manager.apply()

    if args.native_titlebar:
        title_mode = TitleBarMode.NATIVE_FALLBACK
    elif args.expanded_titlebar:
        title_mode = TitleBarMode.EXPANDED_CLIENT_AREA
    else:
        title_mode = TitleBarMode.FRAMELESS
    window = FluentWorkbenchWindow(
        "PySide6 Fluent Workbench Gallery",
        compact=args.compact,
        title_bar_mode=title_mode,
        theme_manager=manager,
    )

    style = app.style()
    home_icon = style.standardIcon(QStyle.StandardPixmap.SP_DirHomeIcon)
    search_icon = style.standardIcon(QStyle.StandardPixmap.SP_FileDialogContentsView)
    settings_icon = style.standardIcon(QStyle.StandardPixmap.SP_FileDialogDetailedView)

    window.add_view(
        "components",
        "Components",
        home_icon,
        component_page(compact=args.compact),
        sidebar=make_sidebar("Gallery", ["Actions", "Inputs", "Feedback", "Data view"]),
        badge=3,
    )
    window.add_view(
        "search",
        "Search",
        search_icon,
        search_page(),
        sidebar=make_sidebar("Search", ["Project", "Open views", "History"]),
    )
    window.add_view(
        "settings",
        "Settings",
        settings_icon,
        settings_page(manager),
        sidebar=make_sidebar("Settings", ["Appearance", "Accessibility", "Workbench"]),
        location="secondary",
    )

    command = QLineEdit()
    command.setPlaceholderText("Search or run a command")
    set_fluent_property(command, "fluentRole", "commandCenter")
    apply_accessible_identity(command, name="Command center", description="Search or run a global command")
    window.title_bar.set_command_widget(command)

    theme_picker = QComboBox()
    theme_picker.setToolTip("Theme")
    for item in ThemeMode:
        theme_picker.addItem(item.value, item)
    theme_picker.setCurrentText(manager.mode.value)
    theme_picker.currentIndexChanged.connect(lambda index: manager.set_mode(theme_picker.itemData(index)))
    window.title_bar.add_trailing_widget(theme_picker)

    help_button = QToolButton()
    help_button.setIcon(style.standardIcon(QStyle.StandardPixmap.SP_MessageBoxQuestion))
    help_button.setToolTip("Help")
    set_fluent_property(help_button, "fluentAppearance", "subtle")
    set_fluent_property(help_button, "fluentIconOnly", True)
    apply_accessible_identity(help_button, name="Help")
    window.title_bar.add_trailing_widget(help_button)

    window.status_bar.add_entry(
        "workspace",
        "main",
        alignment=StatusAlignment.LEFT,
        priority=100,
        tooltip="Current workspace branch: main",
    )
    window.status_bar.add_entry(
        "problems",
        "0 problems",
        alignment=StatusAlignment.LEFT,
        priority=90,
        tooltip="No current problems",
    )
    window.status_bar.add_entry(
        "position",
        "Ln 1, Col 1",
        alignment=StatusAlignment.RIGHT,
        priority=100,
        tooltip="Current cursor position",
    )
    window.status_bar.add_entry(
        "encoding",
        "UTF-8",
        alignment=StatusAlignment.RIGHT,
        priority=80,
        tooltip="Current text encoding",
    )

    window.currentViewChanged.connect(lambda view_id: window.status_bar.update_entry("workspace", text=view_id))
    return window, manager


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--theme",
        choices=[mode.value for mode in ThemeMode],
        default=ThemeMode.SYSTEM.value,
    )
    parser.add_argument("--compact", action="store_true")
    title_group = parser.add_mutually_exclusive_group()
    title_group.add_argument("--native-titlebar", action="store_true")
    title_group.add_argument("--expanded-titlebar", action="store_true")
    parser.add_argument(
        "--shell-profile",
        choices=["fluent-workbench", "fluent-workbench-neutral-status"],
        default="fluent-workbench",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    app = QApplication(sys.argv)
    app.setApplicationName("PySide6 Fluent Workbench Gallery")
    window, _manager = build_window(app, args)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
