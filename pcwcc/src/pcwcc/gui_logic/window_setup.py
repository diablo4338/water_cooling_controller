from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from .constants import APP_TITLE, PARAM_FIELDS
from .metrics_chart import MetricsHistoryChart


class MainWindowSetupMixin:
    _MIN_INPUT_HEIGHT = 30

    def _create_widgets(self) -> None:
        self.setWindowTitle(APP_TITLE)
        if self.debug_enabled:
            self.resize(1280, 720)
        else:
            self.resize(900, 600)
        self.scan_button = QPushButton("Scan")
        self.pair_button = QPushButton("Pair")
        self.connect_button = QPushButton("Connect")
        self.disconnect_button = QPushButton("Disconnect")
        self.delete_button = QPushButton("Delete saved")
        self.rename_button = QPushButton("Rename saved")
        self.auto_checkbox = QCheckBox("Auto-connect (saved)")
        self.apply_button = QPushButton("Apply")
        self.discard_button = QPushButton("Discard")
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
        self.metrics_history_mode_label = QLabel("OFFLINE")
        self.metrics_history_mode_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.metrics_history_mode_label.setStyleSheet(
            "background:#6b7280; color:#ffffff; padding:2px 8px; border-radius:4px;"
        )
        self.status_label = QLabel(self.model.state.status)
        self.apply_status_label = QLabel("")
        self.apply_status_label.setMinimumWidth(220)
        self.apply_status_label.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
        self.apply_status_label.setStyleSheet("color:#6b7280;")
        self._apply_status_timer = QTimer(self)
        self._apply_status_timer.setSingleShot(True)
        self._apply_status_timer.timeout.connect(self._clear_apply_status)

    def _init_view_state(self) -> None:
        self.temp_fields = []
        self.temp_indicators = []
        self.temp_values = [None] * 4
        self._temp_is_nc = [None] * 4
        self.fan_fields = []
        self.fan_percent_field = None
        self._fan_is_nc = [None] * 4
        self.fan_status_indicators = []
        self.fan_monitor_checkboxes = []
        self.voltage_field = None
        self.current_field = None
        self.device_status_field = None
        self.device_status_indicator = None
        self._operation_active = False
        self.param_fields = []
        self._params_update_lock = False
        self._device_params_snapshot = None
        self._apply_status_kind = "idle"
        self._apply_status_text = ""

    def _configure_action_properties(self) -> None:
        for widget, action in (
            (self.scan_button, self.Action.SCAN),
            (self.pair_button, self.Action.PAIR),
            (self.connect_button, self.Action.CONNECT),
            (self.disconnect_button, self.Action.DISCONNECT),
            (self.apply_button, self.Action.APPLY),
            (self.discard_button, self.Action.DISCARD),
            (self.calibrate_button, self.Action.CALIBRATE),
            (self.setup_fans_button, self.Action.SETUP_FANS),
            (self.delete_button, self.Action.DELETE_PAIRED),
            (self.auto_checkbox, self.Action.AUTO_CONNECT),
        ):
            widget.setProperty("actionId", action.name)

    def _build_window(self) -> None:
        buttons = QHBoxLayout()
        buttons.addWidget(self.scan_button)
        buttons.addWidget(self.pair_button)
        buttons.addWidget(self.connect_button)
        buttons.addWidget(self.disconnect_button)
        buttons.addWidget(self.delete_button)
        buttons.addWidget(self.rename_button)
        buttons.addStretch(1)
        buttons.addWidget(self.auto_checkbox)

        lists_layout = QHBoxLayout()
        lists_layout.addWidget(self._wrap_section("Devices for pairing", self.found_list))
        lists_layout.addWidget(self._wrap_section("Paired devices", self.paired_list))

        main_content_layout = QVBoxLayout()
        main_content_layout.addLayout(buttons)
        main_content_layout.addLayout(lists_layout)
        main_content_layout.addWidget(QLabel("Device parameters"))
        main_content_layout.addWidget(self._build_params_layout())

        sensors_layout = QHBoxLayout()
        sensors_layout.addWidget(self._wrap_section("Temperatures", self._build_temp_widget()))
        sensors_layout.addWidget(self._wrap_section("Fan speed", self._build_fan_widget()))
        main_content_layout.addLayout(sensors_layout)

        main_content_layout.addWidget(QLabel("Power"))
        main_content_layout.addLayout(self._build_power_layout())
        main_content_layout.addWidget(self._build_device_status_widget())
        main_content_layout.addLayout(self._build_operation_buttons_layout())
        history_header = QHBoxLayout()
        history_header.addWidget(QLabel("History"))
        history_header.addWidget(self.metrics_history_mode_label)
        history_header.addStretch(1)
        main_content_layout.addLayout(history_header)
        main_content_layout.addWidget(self.metrics_chart)

        main_content_widget = QWidget()
        main_content_widget.setLayout(main_content_layout)

        main_scroll = QScrollArea()
        main_scroll.setWidgetResizable(True)
        main_scroll.setFrameShape(QFrame.Shape.NoFrame)
        main_scroll.setWidget(main_content_widget)

        body_layout = QHBoxLayout()
        body_layout.addWidget(main_scroll, 3)
        if self.debug_enabled:
            body_layout.addWidget(self._build_debug_panel(), 2)

        root_layout = QVBoxLayout()
        root_layout.addLayout(body_layout)
        root_layout.addWidget(self.status_label)

        container = QWidget()
        container.setLayout(root_layout)
        self.setCentralWidget(container)

    def _connect_signals(self) -> None:
        self.scan_button.clicked.connect(self.on_scan)
        self.pair_button.clicked.connect(self.on_pair)
        self.connect_button.clicked.connect(self.on_connect)
        self.disconnect_button.clicked.connect(self.on_disconnect)
        self.delete_button.clicked.connect(self.on_delete_paired_clicked)
        self.rename_button.clicked.connect(self.on_rename_paired_clicked)
        self.auto_checkbox.toggled.connect(self.on_auto_toggled)
        self.apply_button.clicked.connect(self.on_apply)
        self.discard_button.clicked.connect(self.on_discard)
        self.calibrate_button.clicked.connect(self.on_calibrate)
        self.setup_fans_button.clicked.connect(self.on_setup_fans)
        self.clear_op_log_button.clicked.connect(self._clear_op_log)
        self.clear_debug_log_button.clicked.connect(self._clear_debug_log)
        self.clear_data_log_button.clicked.connect(self._clear_data_log)

        self.worker.log.connect(self.on_log)
        self.worker.scan_results.connect(self.on_scan_results)
        self.worker.metrics_received.connect(self.on_metrics_received)
        self.worker.params_received.connect(self.on_params_received)
        self.worker.params_status.connect(self.on_params_status)
        self.worker.apply_done.connect(self.on_apply_done)
        self.worker.fan_status_received.connect(self.on_fan_status)
        self.worker.device_status_received.connect(self.on_device_status)
        self.worker.operation_status_received.connect(self.on_operation_status)
        self.worker.pairing_result.connect(self.on_pairing_result)
        self.worker.connection_state.connect(self.on_connection_state)

        self.found_list.itemSelectionChanged.connect(self._on_found_selected)
        self.paired_list.itemSelectionChanged.connect(self._on_paired_selected)
        self.paired_list.itemChanged.connect(self._on_paired_item_changed)

    def closeEvent(self, event) -> None:
        self.worker.stop_auto()
        self._save_metrics_chart_history()
        fut = self.worker.submit(self.worker.disconnect_device())
        if fut is not None:
            try:
                fut.result(timeout=3)
            except Exception:
                pass
        self.worker.stop()
        self.worker.wait(3000)
        super().closeEvent(event)

    @staticmethod
    def _wrap_section(title: str, widget: QWidget, action_button: QPushButton | None = None) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        layout.addWidget(QLabel(title))
        if action_button is not None:
            action_row = QHBoxLayout()
            action_row.addStretch(1)
            action_row.addWidget(action_button)
            layout.addLayout(action_row)
        layout.addWidget(widget)
        wrapper.setLayout(layout)
        return wrapper

    def _build_temp_widget(self) -> QWidget:
        wrapper = QWidget()
        wrapper.setLayout(self._build_temp_layout())
        return wrapper

    def _build_fan_widget(self) -> QWidget:
        wrapper = QWidget()
        wrapper.setLayout(self._build_fan_layout())
        return wrapper

    def _build_debug_panel(self) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        layout.addWidget(self._wrap_section("Operations (log)", self.op_log_view, self.clear_op_log_button))
        layout.addWidget(self._wrap_section("Debug log", self.debug_log_view, self.clear_debug_log_button))
        layout.addWidget(self._wrap_section("Real-time data", self.data_view, self.clear_data_log_button))
        wrapper.setLayout(layout)
        return wrapper

    def _build_params_layout(self) -> QWidget:
        wrapper = QWidget()
        layout = QVBoxLayout()
        groups_layout = QHBoxLayout()
        self.param_fields.clear()
        for group_key, group_title in (("control", "Control"), ("fan", "Fan")):
            box = QGroupBox(group_title)
            grid = QGridLayout()
            row = 0
            for spec in PARAM_FIELDS:
                if spec.get("group") != group_key:
                    continue
                grid.addWidget(QLabel(spec["label"]), row, 0)
                kind = spec["kind"]
                if kind == "float":
                    field = QDoubleSpinBox()
                    field.setRange(-1_000_000.0, 1_000_000.0)
                    field.setDecimals(2)
                    field.setSingleStep(0.5)
                    field.setAlignment(Qt.AlignmentFlag.AlignRight)
                    field.setKeyboardTracking(False)
                    field.valueChanged.connect(self._on_params_changed)
                    field.setValue(spec.get("min", 0.0))
                elif kind == "int":
                    field = QSpinBox()
                    field.setRange(spec.get("min", -1_000_000), spec.get("max", 1_000_000))
                    field.setSingleStep(1)
                    field.setAlignment(Qt.AlignmentFlag.AlignRight)
                    field.setKeyboardTracking(False)
                    field.valueChanged.connect(self._on_params_changed)
                    field.setValue(spec.get("min", -1_000_000))
                elif kind == "enum":
                    field = QComboBox()
                    for value, label in spec.get("choices", []):
                        field.addItem(label, value)
                    field.currentIndexChanged.connect(self._on_params_changed)
                    field.setCurrentIndex(0)
                else:
                    continue
                self._apply_input_sizing(field)
                field.setEnabled(False)
                grid.addWidget(field, row, 1)
                self.param_fields.append({"spec": spec, "widget": field})
                row += 1
            box.setLayout(grid)
            groups_layout.addWidget(box)

        layout.addLayout(groups_layout)

        button_row = QHBoxLayout()
        button_row.addStretch(1)
        button_row.addWidget(self.apply_status_label)
        button_row.addWidget(self.apply_button)
        button_row.addWidget(self.discard_button)
        layout.addLayout(button_row)
        wrapper.setLayout(layout)
        return wrapper

    def _build_temp_layout(self) -> QGridLayout:
        grid = QGridLayout()
        self.temp_fields.clear()
        self.temp_indicators.clear()
        self.temp_values = [None] * 4
        self._temp_is_nc = [None] * 4
        for idx in range(4):
            label = QLabel(f"Temp {idx + 1}")
            label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            field = QLineEdit("—")
            field.setReadOnly(True)
            field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            field.setCursor(Qt.CursorShape.ArrowCursor)
            field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            field.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self._apply_input_sizing(field)
            indicator = QFrame()
            indicator.setFixedSize(16, 16)
            indicator.setStyleSheet("background:#6b7280; border:1px solid #111827;")
            indicator.setToolTip("—")
            value_row = QHBoxLayout()
            value_row.addWidget(field)
            value_row.addWidget(indicator)

            column = QVBoxLayout()
            column.addWidget(label)
            column.addLayout(value_row)
            grid.addLayout(column, 0, idx)
            self.temp_fields.append(field)
            self.temp_indicators.append(indicator)
        return grid

    def _build_power_layout(self) -> QGridLayout:
        grid = QGridLayout()

        voltage_field = QLineEdit("â€”")
        voltage_field.setReadOnly(True)
        voltage_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        voltage_field.setCursor(Qt.CursorShape.ArrowCursor)
        voltage_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        voltage_field.setAlignment(Qt.AlignmentFlag.AlignRight)
        self._apply_input_sizing(voltage_field)

        current_field = QLineEdit("â€”")
        current_field.setReadOnly(True)
        current_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        current_field.setCursor(Qt.CursorShape.ArrowCursor)
        current_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        current_field.setAlignment(Qt.AlignmentFlag.AlignRight)
        self._apply_input_sizing(current_field)

        grid.addWidget(QLabel("Voltage, V"), 0, 0)
        grid.addWidget(voltage_field, 0, 1)
        grid.addWidget(QLabel("Current, mA"), 0, 2)
        grid.addWidget(current_field, 0, 3)

        self.voltage_field = voltage_field
        self.current_field = current_field
        return grid

    def _build_operation_buttons_layout(self) -> QHBoxLayout:
        layout = QHBoxLayout()
        layout.addWidget(self.setup_fans_button)
        layout.addWidget(self.calibrate_button)
        layout.addStretch(1)
        return layout

    def _build_device_status_widget(self) -> QWidget:
        wrapper = QWidget()
        layout = QHBoxLayout()

        device_status_label = QLabel("Device status")

        device_status_field = QLineEdit("â€”")
        device_status_field.setReadOnly(True)
        device_status_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        device_status_field.setCursor(Qt.CursorShape.ArrowCursor)
        device_status_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        device_status_field.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._apply_input_sizing(device_status_field)
        self.device_status_field = device_status_field

        device_indicator = QFrame()
        device_indicator.setFixedSize(16, 16)
        device_indicator.setStyleSheet("background:#6b7280; border:1px solid #111827;")
        self.device_status_indicator = device_indicator

        value_row = QHBoxLayout()
        value_row.addWidget(device_status_label)
        value_row.addWidget(device_status_field)
        value_row.addWidget(device_indicator)
        value_row.addStretch(1)

        layout.addLayout(value_row)
        wrapper.setLayout(layout)
        return wrapper

    def _build_fan_layout(self) -> QGridLayout:
        grid = QGridLayout()
        self.fan_fields = []
        self.fan_percent_field = None
        self.fan_status_indicators = []
        self.fan_monitor_checkboxes = []
        for idx in range(4):
            label = QLabel(f"Fan{idx + 1}")
            label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            rpm_field = QLineEdit("—")
            rpm_field.setReadOnly(True)
            rpm_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            rpm_field.setCursor(Qt.CursorShape.ArrowCursor)
            rpm_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            rpm_field.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self._apply_input_sizing(rpm_field)
            indicator = QFrame()
            indicator.setFixedSize(16, 16)
            indicator.setStyleSheet("background:#6b7280; border:1px solid #111827;")
            indicator.setToolTip("—")
            monitor = QCheckBox()
            monitor.setToolTip("Monitor faults")
            monitor.setEnabled(False)
            if idx > 0:
                monitor.stateChanged.connect(self._on_params_changed)
            value_row = QHBoxLayout()
            value_row.addWidget(rpm_field)
            value_row.addWidget(indicator)
            value_row.addWidget(monitor)

            column = QVBoxLayout()
            column.addWidget(label)
            column.addLayout(value_row)
            grid.addLayout(column, 0, idx)
            self.fan_fields.append(rpm_field)
            self.fan_status_indicators.append(indicator)
            self.fan_monitor_checkboxes.append(monitor)
        label = QLabel("Speed(%)")
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        percent_field = QLineEdit("â€”")
        percent_field.setReadOnly(True)
        percent_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        percent_field.setCursor(Qt.CursorShape.ArrowCursor)
        percent_field.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        percent_field.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._apply_input_sizing(percent_field)
        column = QVBoxLayout()
        column.addWidget(label)
        column.addWidget(percent_field)
        grid.addLayout(column, 0, 4)
        self.fan_percent_field = percent_field
        self._fan_is_nc = [None] * 4
        return grid

    def _apply_input_sizing(self, widget: QWidget) -> None:
        widget.setMinimumHeight(self._MIN_INPUT_HEIGHT)
