"""Application-wide Qt stylesheet with semantic color placeholders."""

STYLES = """
QWidget[fluentRole="sidebar"] {
    background: ${window_background};
    border-right: 1px solid ${border_subtle};
}
QWidget[uiRole="page"], QStackedWidget, QScrollArea { background: ${canvas_background}; }
QLabel[uiRole="title"] { font-size: 21pt; font-weight: 600; }
QLabel[uiRole="section"], QGroupBox { font-weight: 600; }
QLabel[uiRole="section"] { font-size: 12pt; }
QFrame[fluentRole="card"] {
    background: ${window_background};
    border: 1px solid ${border_subtle}; border-radius: 10px;
}
QWidget[uiRole="settingsContent"] QLabel,
QWidget[uiRole="settingsContent"] QAbstractButton,
QWidget[uiRole="settingsContent"] QAbstractSpinBox,
QWidget[uiRole="settingsContent"] QLineEdit { font-size: 10.5pt; }
QWidget[uiRole="settingsContent"] QLabel[uiRole="section"] { font-size: 12pt; }
QWidget[uiRole="settingsContent"] QLabel[uiRole="panelTitle"] { font-size: 15pt; font-weight: 600; }
QLabel[uiRole="muted"], QLabel[uiRole="feedback"] { color: ${text_secondary}; }
QLabel[uiRole="feedback"][fluentSeverity="success"] { color: ${success_foreground}; }
QLabel[uiRole="feedback"][fluentSeverity="danger"] { color: ${danger_foreground}; }
QLabel[uiRole="warning"] {
    color: ${warning_foreground}; background: ${warning_background};
    border-radius: 6px; padding: 8px 12px;
}
QLabel[uiRole="error"] {
    color: ${danger_foreground}; border: 1px solid ${danger_foreground};
    border-radius: 6px; padding: 8px 12px;
}
QLabel[uiRole="historyMode"] {
    color: ${text_secondary}; background: ${surface_tertiary};
    border-radius: 4px; padding: 3px 8px;
}
QLabel[uiRole="historyMode"][fluentSeverity="success"] {
    color: ${success_foreground}; background: ${success_background};
}
QFrame[uiRole="indicator"] { border-radius: 4px; background: ${border_accessible}; }
QFrame[uiRole="indicator"][fluentSeverity="success"] { background: ${success_foreground}; }
QFrame[uiRole="indicator"][fluentSeverity="warning"] { background: ${warning_foreground}; }
QFrame[uiRole="indicator"][fluentSeverity="danger"] { background: ${danger_foreground}; }
QFrame[uiRole="indicator"][fluentSeverity="info"] { background: ${info_foreground}; }

QPushButton {
    background: ${canvas_background}; color: ${text_primary};
    border: 1px solid ${border_accessible}; border-radius: 6px;
    padding: 5px 11px; min-height: 19px;
}
QPushButton:hover { background: ${surface_background_hover}; }
QPushButton:pressed { background: ${surface_background_pressed}; }
QPushButton[fluentAppearance="primary"], QPushButton:checked {
    background: ${brand_background}; color: ${text_on_brand}; border-color: ${brand_background};
}
QPushButton[fluentAppearance="primary"]:hover, QPushButton:checked:hover {
    background: ${brand_background_hover};
}
QPushButton[fluentAppearance="primary"]:pressed, QPushButton:checked:pressed {
    background: ${brand_background_pressed};
}
QPushButton[fluentRole="activityItem"] {
    text-align: left; padding: 7px 12px; border: 1px solid transparent;
    background: transparent; color: ${text_primary};
}
QPushButton[fluentRole="activityItem"]:hover { background: ${surface_background_hover}; }
QPushButton[fluentRole="activityItem"]:pressed { background: ${surface_background_pressed}; }
QPushButton[fluentRole="activityItem"]:checked {
    background: ${selection_subtle_background}; color: ${text_primary};
    border-left: 3px solid ${brand_foreground}; padding-left: 10px; font-weight: 600;
}
QComboBox, QAbstractSpinBox { min-height: 28px; }
QSlider { min-height: 26px; border: 1px solid transparent; border-radius: 4px; }
QSlider:focus { border-color: ${focus_border}; }
QSlider::groove:horizontal {
    height: 5px; border-radius: 2px; background: ${border_primary};
}
QSlider::sub-page:horizontal { border-radius: 2px; background: ${brand_foreground}; }
QSlider::handle:horizontal {
    width: 14px; margin: -5px 0; border-radius: 7px;
    background: ${canvas_background}; border: 2px solid ${brand_foreground};
}
QSlider::handle:horizontal:hover { background: ${selection_subtle_background}; }
QSlider::handle:horizontal:pressed { background: ${brand_foreground}; }
QSlider::sub-page:horizontal:disabled { background: ${border_disabled}; }
QSlider::handle:horizontal:disabled {
    background: ${surface_disabled}; border-color: ${border_disabled};
}
QCheckBox { spacing: 8px; padding: 2px; min-height: 24px; border: 1px solid transparent; }
QListWidget, QTextEdit, QLineEdit {
    background: ${canvas_background}; border: 1px solid ${border_accessible}; border-radius: 6px;
}
QLineEdit { padding: 3px 5px; }
QLineEdit[uiRole="reading"] {
    background: ${surface_tertiary}; border-color: ${border_subtle};
    font-weight: 600; padding: 5px 6px;
}
QAbstractSpinBox QLineEdit, QComboBox QLineEdit {
    border: none; padding: 0; background: transparent;
}
QListWidget, QTextEdit { padding: 4px; }
QListWidget::item { padding: 6px 8px; }
QListWidget::item:hover:!selected { background: ${surface_background_hover}; }
QListWidget::item:selected { background: ${brand_background}; color: ${text_on_brand}; }
QPushButton:focus, QPushButton[fluentRole="activityItem"]:focus, QCheckBox:focus,
QListWidget:focus, QTextEdit:focus, QLineEdit:focus {
    border-color: ${focus_border};
}
QPushButton:disabled, QLineEdit:disabled {
    background: ${surface_disabled}; color: ${text_disabled}; border-color: ${border_disabled};
}
QStatusBar { background: ${window_background}; border-top: 1px solid ${border_subtle}; }
QStatusBar::item { border: 0; }
QToolTip { background: ${tooltip_background}; color: ${tooltip_foreground}; border: 0; padding: 4px; }
"""
