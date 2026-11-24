from __future__ import annotations

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QActionGroup
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QStackedWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from ..core import FAN_MODE_CONTINUOUS, FAN_MODE_TEMP_SENSOR, PROFILE_COUNT
from ..gui_logic.constants import APP_TITLE, PARAM_FIELDS
from ..gui_logic.metrics_chart import MetricsHistoryChart


from .inline_message import InlineMessage
from .parameter_controls import ParameterChoices, ParameterNumber
from .theme import FluentThemeManager, describe


if TYPE_CHECKING:
    from ..gui_logic.app import MainWindow


class MainWindowViewMixin:
    def _create_widgets(self: MainWindow) -> None:
        self.setWindowTitle(APP_TITLE)
        if self.debug_enabled:
            self.resize(1120, 800)
        else:
            self.resize(1024, 760)
        self.scan_button = QPushButton("Scan")
        self.pair_button = QPushButton("Pair")
        self.connect_button = QPushButton("Connect")
        self.disconnect_button = QPushButton("Disconnect")
        self.delete_button = QPushButton("Delete saved")
        self.rename_button = QPushButton("Rename saved")
        self.auto_checkbox = QCheckBox("Auto-connect (saved)")
        self.apply_button = QPushButton("Apply")
        self.apply_button.setFocusPolicy(Qt.FocusPolicy.TabFocus)
        for button in (self.scan_button, self.connect_button, self.apply_button):
            button.setProperty("fluentAppearance", "primary")
        self.discard_button = QPushButton("Discard")
        self.clear_profile_button = QPushButton("Clear")
        self.clear_profile_button.setToolTip("Remove the saved settings from the selected profile")
        self.clear_profile_button.clicked.connect(self.on_clear_profile)
        self.profile_button_group = QButtonGroup(self)
        self.profile_button_group.setExclusive(False)
        self.profile_buttons = []
        self.profile_status_bars = []
        self.profile_button_cells = []
        for index in range(1, PROFILE_COUNT + 1):
            button = QPushButton(f"P{index}")
            button.setCheckable(True)
            button.setMinimumWidth(42)
            button.setAccessibleName(f"Profile {index}")
            button.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            button.clicked.connect(
                lambda checked=False, profile_index=index: self.on_profile_clicked(
                    profile_index, checked
                )
            )
            button.customContextMenuRequested.connect(
                lambda position, profile_index=index, profile_button=button: self._show_profile_menu(
                    profile_index, profile_button.mapToGlobal(position)
                )
            )
            self.profile_button_group.addButton(button, index)
            self.profile_buttons.append(button)
            status_bar = QFrame()
            status_bar.setFixedHeight(4)
            status_bar.setProperty("uiRole", "indicator")
            self.profile_status_bars.append(status_bar)
            cell = QWidget()
            cell_layout = QVBoxLayout(cell)
            cell_layout.setContentsMargins(0, 0, 0, 0)
            cell_layout.setSpacing(3)
            cell_layout.addWidget(button)
            cell_layout.addWidget(status_bar)
            self.profile_button_cells.append(cell)
        self.calibrate_button = QPushButton("Fan calibration")
        self.setup_fans_button = QPushButton("Setup fans")
        self.found_list = QListWidget()
        self.paired_list = QListWidget()
        self.data_view = QTextEdit()
        self.data_view.setReadOnly(True)
        self.op_log_view = QTextEdit()
        self.op_log_view.setReadOnly(True)
        self.debug_log_view = QTextEdit()
        self.debug_log_view.setReadOnly(True)
        self.clear_op_log_button = QPushButton("Clear")
        self.clear_debug_log_button = QPushButton("Clear")
        self.clear_data_log_button = QPushButton("Clear")
        self.metrics_chart = MetricsHistoryChart()
        self.metrics_chart.setAccessibleName("Temperature, fan speed and output history")
        self.metrics_history_mode_label = QLabel("OFFLINE")
        self.metrics_history_mode_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.metrics_history_mode_label.setProperty("uiRole", "historyMode")
        self.status_label = QLabel(self.model.state.status)
        self.apply_status_label = QLabel("")
        self.apply_status_label.setWordWrap(True)
        self.apply_status_label.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
        self.apply_status_label.setProperty("uiRole", "feedback")
        self.apply_status_label.hide()
        self._apply_status_timer = QTimer(self)
        self._apply_status_timer.setSingleShot(True)
        self._apply_status_timer.timeout.connect(self._clear_apply_status)

    def _build_window(self: MainWindow) -> None:
        theme = FluentThemeManager.for_app()
        view_menu = self.menuBar().addMenu("&View")
        appearance = view_menu.addMenu("&Appearance")
        modes = QActionGroup(self)
        for theme_mode, label in (
            ("system", "Follow system"), ("light", "Light"), ("dark", "Dark"),
            ("contrast", "System contrast"),
        ):
            action = appearance.addAction(label)
            action.setCheckable(True)
            action.setChecked(theme.mode == theme_mode)
            modes.addAction(action)
            action.triggered.connect(lambda _checked=False, mode=theme_mode: theme.set_mode(mode))
        appearance.addSeparator()
        motion = appearance.addAction("Reduce motion")
        motion.setCheckable(True)
        motion.setChecked(theme.reduced_motion)
        motion.toggled.connect(theme.set_reduced_motion)
        theme.theme_changed.connect(self._apply_paired_highlight)
        theme.theme_changed.connect(self.metrics_chart.refresh_theme)

        sidebar = QWidget()
        sidebar.setAttribute(Qt.WidgetAttribute.WA_StyledBackground)
        sidebar.setProperty("fluentRole", "sidebar")
        sidebar.setMinimumWidth(136)
        navigation = QVBoxLayout(sidebar)
        navigation.setContentsMargins(12, 24, 12, 16)
        navigation.setSpacing(6)
        navigation.addWidget(self._label("PCWCC", "section"))
        navigation.addSpacing(16)

        self.pages = QStackedWidget()
        self.navigation = QButtonGroup(self)
        destinations = [
            ("&Devices", self._devices_page),
            ("&Monitoring", self._monitoring_page),
            ("&Settings", self._settings_page),
        ]
        if self.debug_enabled:
            destinations.append(("D&iagnostics", self._diagnostics_page))

        for index, (caption, build_page) in enumerate(destinations):
            button = QPushButton(caption)
            title = caption.replace("&", "")
            button.setAccessibleName(title)
            button.setCheckable(True)
            button.setProperty("fluentRole", "activityItem")
            self.navigation.addButton(button, index)
            navigation.addWidget(button)
            page = build_page()
            scroll = QScrollArea()
            scroll.setAccessibleName(title)
            scroll.setFrameShape(QFrame.Shape.NoFrame)
            scroll.setWidgetResizable(True)
            if build_page in (self._monitoring_page, self._settings_page):
                # Inline messages can cross the scroll threshold while animating.
                scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOn)
            if build_page == self._settings_page:
                self._settings_scroll = scroll
            scroll.setWidget(page)
            self.pages.addWidget(scroll)
        self.navigation.button(0).setChecked(True)
        self.navigation.idClicked.connect(self.pages.setCurrentIndex)
        navigation.addStretch(1)

        body = QWidget()
        layout = QHBoxLayout(body)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(sidebar)
        layout.addWidget(self.pages, 1)
        self.setCentralWidget(body)
        self.statusBar().addWidget(self.status_label, 1)
        self.status_label.setContentsMargins(12, 4, 0, 4)
        self.status_label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)

    @staticmethod
    def _label(text: str, role: str) -> QLabel:
        label = QLabel(text)
        label.setProperty("uiRole", role)
        return label

    def _page(self: MainWindow, title: str, *heading_widgets: QWidget) -> tuple[QWidget, QVBoxLayout]:
        page = QWidget()
        page.setProperty("uiRole", "page")
        layout = QVBoxLayout(page)
        layout.setContentsMargins(24, 20, 24, 24)
        layout.setSpacing(16)
        heading = QHBoxLayout()
        heading.setSpacing(10)
        heading.addWidget(self._label(title, "title"))
        for widget in heading_widgets:
            heading.addWidget(widget, alignment=Qt.AlignmentFlag.AlignVCenter)
        heading.addStretch()
        layout.addLayout(heading)
        return page, layout

    def _devices_page(self: MainWindow) -> QWidget:
        page, layout = self._page("Devices")
        commands = QHBoxLayout()
        commands.setSpacing(8)
        for button in (self.scan_button, self.pair_button, self.connect_button, self.disconnect_button):
            commands.addWidget(button)
        commands.addStretch()
        layout.addLayout(commands)

        lists = QHBoxLayout()
        lists.setSpacing(16)
        lists.addWidget(self._wrap_section("Available for pairing", self.found_list), 1)
        lists.addWidget(self._wrap_section("Saved devices", self.paired_list), 1)
        layout.addLayout(lists, 1)

        saved_actions = QHBoxLayout()
        saved_actions.setSpacing(8)
        saved_actions.addWidget(self.auto_checkbox)
        saved_actions.addStretch()
        saved_actions.addWidget(self.rename_button)
        saved_actions.addWidget(self.delete_button)
        layout.addLayout(saved_actions)
        return page

    def _monitoring_page(self: MainWindow) -> QWidget:
        self.device_status_indicator = QFrame()
        self.device_status_indicator.setFixedSize(14, 14)
        self.device_status_indicator.setProperty("uiRole", "indicator")
        self.device_status_indicator.setAccessibleName("Controller status")
        describe(self.device_status_indicator, "Unavailable")
        page, layout = self._page("Monitoring", self.metrics_history_mode_label)
        layout.setSpacing(12)
        self.device_status_field = self._label("", "deviceStatus")
        self.device_status_field.setWordWrap(True)
        self.device_status_field.hide()
        controller_status = QHBoxLayout()
        controller_status.setSpacing(8)
        controller_status.addWidget(self._label("Controller status", "deviceStatus"), alignment=Qt.AlignmentFlag.AlignTop)
        controller_status.addWidget(self.device_status_indicator, alignment=Qt.AlignmentFlag.AlignTop)
        controller_status.addWidget(self.device_status_field, 1)
        controller_status.addStretch()
        layout.addLayout(controller_status)

        readings_panel = QWidget()
        readings_panel.setProperty("uiRole", "monitorReadings")
        readings = QHBoxLayout()
        readings.setContentsMargins(0, 0, 0, 0)
        readings.setSpacing(16)
        for index, (title, widget) in enumerate((
            ("Temperatures", self._build_temp_widget()),
            ("Fan speed", self._build_fan_widget()),
            ("Power", self._build_power_widget()),
        )):
            if index:
                divider = QFrame()
                divider.setFrameShape(QFrame.Shape.VLine)
                divider.setProperty("uiRole", "monitorDivider")
                readings.addWidget(divider)
            readings.addWidget(self._wrap_section(title, widget), alignment=Qt.AlignmentFlag.AlignTop)
        readings_panel.setLayout(readings)
        layout.addWidget(readings_panel, alignment=Qt.AlignmentFlag.AlignLeft)
        self._ina_warning_text = (
            "INA226 unavailable. Load monitoring is unavailable. Adjust the output at your own risk."
        )
        self.ina_warning_label = self._label("", "warning")
        self.ina_warning_label.setWordWrap(True)
        self.ina_warning_label.hide()
        self.ina_warning_message = InlineMessage(self.ina_warning_label)
        layout.addWidget(self.ina_warning_message)
        self.output_limit_label = self._label("", "error")
        self.output_limit_label.setWordWrap(True)
        self.output_limit_label.hide()
        self.output_limit_message = InlineMessage(self.output_limit_label)
        layout.addWidget(self.output_limit_message)
        layout.addWidget(self.metrics_chart, 1)
        return page

    def _settings_page(self: MainWindow) -> QWidget:
        page, layout = self._page("Settings")
        content = QWidget()
        content.setProperty("uiRole", "settingsContent")
        content.setMaximumWidth(960)
        font = page.font()
        font.setPointSizeF(max(10.5, font.pointSizeF()))
        content.setFont(font)
        panels = QVBoxLayout(content)
        panels.setContentsMargins(0, 0, 0, 0)
        panels.setSpacing(20)

        parameters = self._add_settings_panel(panels, "Operating mode")
        parameters.addLayout(self._build_params_layout())
        parameters.addWidget(self._wrap_section("Fan fault monitoring", self._build_fan_monitoring_widget()))
        self.apply_status_message = InlineMessage(self.apply_status_label)
        parameters.addWidget(self.apply_status_message)

        maintenance = self._add_settings_panel(panels, "Fan maintenance")
        maintenance.addLayout(self._build_operation_buttons_layout())
        limit = self._add_settings_panel(panels, "Output limit")
        limit.addLayout(self._build_output_limit_layout())

        content_row = QHBoxLayout()
        content_row.setSpacing(0)
        content_row.addWidget(content, 1)
        content_row.addStretch()
        layout.addLayout(content_row)
        layout.addStretch()
        return page

    def _add_settings_panel(self: MainWindow, panels: QVBoxLayout, title: str) -> QVBoxLayout:
        panel = QFrame()
        panel.setAccessibleName(title)
        panel.setProperty("fluentRole", "card")
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(20, 16, 20, 16)
        layout.setSpacing(12)
        layout.addWidget(self._label(title, "panelTitle"))
        panels.addWidget(panel)
        return layout

    def _apply_settings_in_place(self: MainWindow) -> None:
        scrollbar = self._settings_scroll.verticalScrollBar()
        position = scrollbar.value()
        self.on_apply()
        # Pending writes disable form controls, which can move focus down the page.
        scrollbar.setValue(position)

    def _diagnostics_page(self: MainWindow) -> QWidget:
        page, layout = self._page("Diagnostics")
        layout.addWidget(self._build_debug_panel(), 1)
        return page

    def _wrap_section(self: MainWindow, title: str, widget: QWidget, action_button: QPushButton | None = None) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout(wrapper)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)
        heading = QHBoxLayout()
        heading.addWidget(self._label(title, "section"))
        heading.addStretch()
        if action_button is not None:
            action_button.setAccessibleName(f"Clear {title.lower()}")
            heading.addWidget(action_button)
        layout.addLayout(heading)
        widget.setAccessibleName(title)
        layout.addWidget(widget)
        return wrapper

    def _build_debug_panel(self: MainWindow) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addWidget(self._wrap_section("Operations (log)", self.op_log_view, self.clear_op_log_button))
        layout.addWidget(self._wrap_section("Debug log", self.debug_log_view, self.clear_debug_log_button))
        layout.addWidget(self._wrap_section("Real-time data", self.data_view, self.clear_data_log_button))
        wrapper.setLayout(layout)
        return wrapper

    def _build_params_layout(self: MainWindow) -> QVBoxLayout:
        layout = QVBoxLayout()
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(16)
        layout.addWidget(self._build_device_parameters())
        self.effective_fan_mode_label = self._label("", "muted")
        self.effective_fan_mode_label.setWordWrap(True)
        self.effective_fan_mode_message = InlineMessage(self.effective_fan_mode_label)
        layout.addWidget(self.effective_fan_mode_message)

        button_row = QHBoxLayout()
        button_row.setSpacing(8)
        layout.addWidget(self._label("Profiles", "section"))
        for cell in self.profile_button_cells:
            button_row.addWidget(cell)
        button_row.addWidget(self.clear_profile_button)
        button_row.addStretch(1)
        layout.addLayout(button_row)
        layout.addLayout(self._build_alarm_layout())
        return layout

    def _build_output_limit_layout(self: MainWindow) -> QHBoxLayout:
        limit_layout = QHBoxLayout()
        limit_layout.setSpacing(12)
        limit_label = QLabel("Maximum output")
        limit_layout.addWidget(limit_label)
        self.output_max_input = ParameterNumber(
            limit_label.text(), 0, 100, (1, 100)
        )
        limit_label.setBuddy(self.output_max_input.input)
        self.output_max_input.input.setSpecialValueText("Unavailable")
        self.output_max_input.input.setSuffix("%")
        limit_layout.addWidget(self.output_max_input, 1)
        self.output_max_save = QPushButton("Save maximum")
        self.output_max_save.clicked.connect(lambda: self.on_change_output_limit(self.output_max_input.value()))
        self.output_max_input.valueChanged.connect(self._update_output_max_save)
        limit_layout.addWidget(self.output_max_save)
        self.output_limit_reset = QPushButton("Reset overload")
        self.output_limit_reset.setToolTip("Clear the temporary output limit and the overload alarm.")
        self.output_limit_reset.clicked.connect(lambda: self.on_change_output_limit(None))
        limit_layout.addWidget(self.output_limit_reset)
        return limit_layout

    def _build_alarm_layout(self: MainWindow) -> QVBoxLayout:
        spec = next(spec for spec in PARAM_FIELDS if spec["key"] == "overheat_alarm_enabled")
        alarm = QCheckBox(spec["label"])
        alarm.stateChanged.connect(self._on_params_changed)
        alarm.setEnabled(False)
        self.param_fields.append({"spec": spec, "widget": alarm})
        layout = QVBoxLayout()
        layout.addWidget(self._label("Alarms", "section"))
        layout.addWidget(alarm)
        return layout

    def _build_device_parameters(self: MainWindow) -> QWidget:
        specs = {spec["key"]: spec for spec in PARAM_FIELDS}
        self.param_fields.clear()
        mode_spec = specs["fan_mode"]
        mode = ParameterChoices(mode_spec["label"], mode_spec["choices"])
        mode.setEnabled(False)
        self.param_fields.append({"spec": specs["fan_mode"], "widget": mode})

        grid = QGridLayout()
        grid.setHorizontalSpacing(16)
        grid.setVerticalSpacing(8)
        grid.setColumnStretch(1, 1)
        rows = []
        for key, slider_range in (
            ("fan_max_temp", (20, 55)),
            ("fan_start_temp", (20, 55)),
            ("fan_off_delta", (1, 5)),
            ("fan_min_speed", (10, 100)),
        ):
            spec = specs[key]
            field = ParameterNumber(spec["label"], spec["min"], spec["max"], slider_range)
            field.valueChanged.connect(self._on_params_changed)
            field.setEnabled(False)
            self.param_fields.append({"spec": spec, "widget": field})
            label = QLabel(spec["label"])
            label.setBuddy(field.input)
            for widget in (label, field):
                policy = widget.sizePolicy()
                policy.setRetainSizeWhenHidden(True)
                widget.setSizePolicy(policy)
            rows.append((label, field))

        def select_mode(value: int) -> None:
            for index, (row_label, row_input) in enumerate(rows):
                base_speed = index == len(rows) - 1
                visible = value == FAN_MODE_TEMP_SENSOR or (base_speed and value == FAN_MODE_CONTINUOUS)
                row = (index + 1) % len(rows) if value == FAN_MODE_CONTINUOUS else index
                grid.addWidget(row_label, row, 0)
                grid.addWidget(row_input, row, 1)
                row_label.setVisible(visible)
                row_input.setVisible(visible)

        mode.valueChanged.connect(select_mode)
        mode.valueChanged.connect(self._on_params_changed)

        control_spec = specs["fan_control_type"]
        control_type = ParameterChoices(control_spec["label"], control_spec["choices"])
        control_type.valueChanged.connect(self._on_params_changed)
        control_type.setEnabled(False)
        self.param_fields.append({"spec": specs["fan_control_type"], "widget": control_type})

        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addWidget(mode)
        layout.addLayout(grid)
        layout.addWidget(self._label("Fan control type", "section"))
        layout.addWidget(control_type)
        select_mode(mode.value())
        return widget

    def _reading_field(self: MainWindow, accessible_name: str) -> QLabel:
        field = self._label("\u2014", "metricReading")
        field.setAccessibleName(accessible_name)
        field.setAlignment(Qt.AlignmentFlag.AlignRight)
        return field

    @staticmethod
    def _reading_block() -> tuple[QWidget, QGridLayout]:
        widget = QWidget()
        rows = QGridLayout(widget)
        rows.setContentsMargins(0, 0, 0, 0)
        rows.setHorizontalSpacing(7)
        rows.setVerticalSpacing(3)
        rows.setColumnStretch(1, 1)
        return widget, rows

    def _build_temp_widget(self: MainWindow) -> QWidget:
        widget, rows = self._reading_block()
        for index in range(4):
            field, indicator = self._add_sensor_row(rows, index, f"Sensor {index + 1}", f"Temperature sensor {index + 1} in Celsius")
            self.temp_fields.append(field)
            self.temp_indicators.append(indicator)
        return widget

    def _build_power_widget(self: MainWindow) -> QWidget:
        widget, rows = self._reading_block()
        self.voltage_field = self._reading_field("Voltage in volts")
        self.current_field = self._reading_field("Current in milliamps")
        for index, (name, field, unit) in enumerate((
            ("Voltage", self.voltage_field, "V"),
            ("Current", self.current_field, "mA"),
        )):
            rows.addWidget(self._label(name, "muted"), index, 0)
            rows.addWidget(field, index, 1)
            rows.addWidget(self._label(unit, "muted"), index, 2)
        return widget

    def _add_sensor_row(
        self: MainWindow, rows: QGridLayout, index: int, name: str, accessible_name: str
    ) -> tuple[QLabel, QFrame]:
        field = self._reading_field(accessible_name)
        indicator = QFrame()
        indicator.setFixedSize(8, 8)
        indicator.setProperty("uiRole", "indicator")
        describe(indicator, "Unavailable")
        rows.addWidget(self._label(name, "muted"), index, 0)
        rows.addWidget(field, index, 1)
        rows.addWidget(indicator, index, 2, Qt.AlignmentFlag.AlignVCenter)
        return field, indicator

    def _build_operation_buttons_layout(self: MainWindow) -> QHBoxLayout:
        layout = QHBoxLayout()
        layout.setSpacing(12)
        layout.addWidget(self.setup_fans_button)
        layout.addWidget(self.calibrate_button)
        layout.addStretch(1)
        return layout

    def _build_fan_widget(self: MainWindow) -> QWidget:
        widget, rows = self._reading_block()
        for index in range(4):
            name = f"Fan {index + 1}"
            field, indicator = self._add_sensor_row(rows, index, name, f"{name} RPM")
            self.fan_fields.append(field)
            self.fan_status_indicators.append(indicator)
        self.fan_percent_field = self._reading_field("Fan output percent")
        rows.addWidget(self._label("Output", "muted"), 4, 0)
        rows.addWidget(self.fan_percent_field, 4, 1)
        return widget

    def _build_fan_monitoring_widget(self: MainWindow) -> QWidget:
        widget = QWidget()
        layout = QHBoxLayout(widget)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        for index in range(4):
            monitor = QCheckBox(f"Fan {index + 1}")
            monitor.setToolTip("Monitor faults")
            monitor.setAccessibleName(f"Monitor fan {index + 1} faults")
            monitor.setEnabled(False)
            if index > 0:
                monitor.stateChanged.connect(self._on_params_changed)
            layout.addWidget(monitor)
            self.fan_monitor_checkboxes.append(monitor)
        layout.addStretch()
        self.apply_button.setToolTip("Apply changes to device parameters, alarms, and fan fault monitoring")
        layout.addWidget(self.apply_button)
        layout.addWidget(self.discard_button)
        return widget
