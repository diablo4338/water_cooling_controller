from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush, QColor

from ..core import (
    FAN_CONTROL_DC,
    FAN_CONTROL_PWM,
    FAN_CONTROL_TYPE_NAMES,
    FAN_MODE_CONTINUOUS,
    FAN_MODE_INACTIVE,
    FAN_MODE_NAMES,
    FAN_MODE_TEMP_SENSOR,
)

APP_TITLE = "BLE Cooling Controller"
USER_ROLE = Qt.ItemDataRole.UserRole
PAIRED_HIGHLIGHT_BRUSH = QBrush(QColor(220, 245, 220))

FAN_CONTROL_CHOICES = [
    (FAN_CONTROL_DC, FAN_CONTROL_TYPE_NAMES[FAN_CONTROL_DC]),
    (FAN_CONTROL_PWM, FAN_CONTROL_TYPE_NAMES[FAN_CONTROL_PWM]),
]
FAN_MODE_CHOICES = [
    (FAN_MODE_CONTINUOUS, FAN_MODE_NAMES[FAN_MODE_CONTINUOUS]),
    (FAN_MODE_TEMP_SENSOR, FAN_MODE_NAMES[FAN_MODE_TEMP_SENSOR]),
    (FAN_MODE_INACTIVE, FAN_MODE_NAMES[FAN_MODE_INACTIVE]),
]
PARAM_FIELDS = [
    {
        "key": "fan_min_speed",
        "label": "Min fan speed, %",
        "kind": "int",
        "min": 10,
        "max": 100,
        "group": "fan",
    },
    {
        "key": "fan_control_type",
        "label": "Control type",
        "kind": "enum",
        "choices": FAN_CONTROL_CHOICES,
        "group": "fan",
    },
    {
        "key": "fan_max_temp",
        "label": "Max temperature, °C",
        "kind": "int",
        "min": 0,
        "max": 150,
        "group": "control",
    },
    {
        "key": "fan_off_delta",
        "label": "Shutdown delta, °C",
        "kind": "int",
        "min": 0,
        "max": 150,
        "group": "control",
    },
    {
        "key": "fan_start_temp",
        "label": "Start temperature, °C",
        "kind": "int",
        "min": 0,
        "max": 150,
        "group": "control",
    },
    {
        "key": "fan_mode",
        "label": "Mode",
        "kind": "enum",
        "choices": FAN_MODE_CHOICES,
        "group": "control",
    },
]
FAN_MONITORING_KEYS = [
    "fan_monitoring_enabled",
    "fan2_monitoring_enabled",
    "fan3_monitoring_enabled",
    "fan4_monitoring_enabled",
]
PARAM_LABELS_BY_ID = {
    0: "Min fan speed, %",
    1: "Control type",
    2: "Max temperature, °C",
    3: "Shutdown delta, °C",
    4: "Start temperature, °C",
    5: "Mode",
    6: "Monitor fan 1 faults",
    7: "Monitor fan 2 faults",
    8: "Monitor fan 3 faults",
    9: "Monitor fan 4 faults",
}
PARAM_ERROR_MESSAGES = {
    0: "Min fan speed must be between 10 and 100",
    1: "Control type must be DC or PWM",
    2: "Max temperature must be in range 0..150 and greater than start temperature",
    3: "Shutdown delta must be in range 0..150 and less than start temperature",
    4: "Start temperature must be in range 0..150",
    5: "Mode must be Always on, By temperature sensor, or Inactive (fans off)",
    6: "Monitor fan 1 faults must be on or off",
    7: "Monitor fan 2 faults must be on or off",
    8: "Monitor fan 3 faults must be on or off",
    9: "Monitor fan 4 faults must be on or off",
}
