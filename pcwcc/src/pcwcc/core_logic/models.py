from __future__ import annotations

from enum import IntEnum
from dataclasses import dataclass, field
from typing import Any, Optional

from .protocol import (
    DEVICE_STATE_NAMES,
    FAN_CONTROL_DC,
    FAN_MODE_CONTINUOUS,
    FAN_STATE_IDLE,
    FAN_STATE_NAMES,
    OP_TYPE_NAMES,
)


@dataclass(frozen=True)
class DeviceParams:
    fan_min_speed: int
    fan_control_type: int
    fan_max_temp: int
    fan_off_delta: int
    fan_start_temp: int
    fan_mode: int
    fan_monitoring_enabled: bool
    fan2_monitoring_enabled: bool
    fan3_monitoring_enabled: bool
    fan4_monitoring_enabled: bool
    overheat_alarm_enabled: bool
    protocol_version: int = 6


@dataclass(frozen=True)
class MetricsSnapshot:
    temperatures: tuple[Optional[float], Optional[float], Optional[float], Optional[float]]
    fan_speeds: tuple[Optional[float], Optional[float], Optional[float], Optional[float]]
    voltage_v: Optional[float] = None
    current_ma: Optional[float] = None
    fan_percent: Optional[float] = None

    def with_temperature(self, channel: int, value: Optional[float]) -> "MetricsSnapshot":
        if channel < 0 or channel >= len(self.temperatures):
            return self
        temperatures = list(self.temperatures)
        temperatures[channel] = value
        return MetricsSnapshot(tuple(temperatures), self.fan_speeds, self.voltage_v, self.current_ma, self.fan_percent)

    def with_fan_speed(self, channel: int, value: Optional[float]) -> "MetricsSnapshot":
        if channel < 0 or channel >= len(self.fan_speeds):
            return self
        fan_speeds = list(self.fan_speeds)
        fan_speeds[channel] = value
        return MetricsSnapshot(self.temperatures, tuple(fan_speeds), self.voltage_v, self.current_ma, self.fan_percent)

    def with_voltage(self, value: Optional[float]) -> "MetricsSnapshot":
        return MetricsSnapshot(self.temperatures, self.fan_speeds, value, self.current_ma, self.fan_percent)

    def with_current(self, value: Optional[float]) -> "MetricsSnapshot":
        return MetricsSnapshot(self.temperatures, self.fan_speeds, self.voltage_v, value, self.fan_percent)

    def with_fan_percent(self, value: Optional[float]) -> "MetricsSnapshot":
        return MetricsSnapshot(self.temperatures, self.fan_speeds, self.voltage_v, self.current_ma, value)

    @classmethod
    def empty(cls) -> "MetricsSnapshot":
        return cls(
            temperatures=(None, None, None, None),
            fan_speeds=(None, None, None, None),
            voltage_v=None,
            current_ma=None,
            fan_percent=None,
        )


@dataclass(frozen=True)
class ParamsStatus:
    ok: bool
    status: int
    field_id: Optional[int]


@dataclass(frozen=True)
class FanStatus:
    states: tuple[int, int, int, int]
    labels: tuple[str, str, str, str]
    op_type: int
    op_label: str

    @property
    def state(self) -> int:
        if 3 in self.states:
            return 3
        if 4 in self.states:
            return 4
        if 1 in self.states:
            return 1
        if 2 in self.states:
            return 2
        return FAN_STATE_IDLE

    @property
    def label(self) -> str:
        return FAN_STATE_NAMES.get(self.state, "UNKNOWN")


@dataclass(frozen=True)
class DeviceStatus:
    state: int
    label: str
    error_mask: int
    errors: tuple[int, ...]
    error_labels: tuple[str, ...]


@dataclass(frozen=True)
class OperationStatus:
    op_type: int
    state: int
    error: str


@dataclass(frozen=True)
class DeviceInfo:
    name: str
    address: str
    ble_device: Any = field(default=None, compare=False, repr=False)

    def with_ble_device(self, ble_device: Any) -> "DeviceInfo":
        return DeviceInfo(name=self.name, address=self.address, ble_device=ble_device)


@dataclass(frozen=True)
class PairResult:
    ok: bool
    message: str
    k_hex: Optional[str]


DEFAULT_PARAMS = DeviceParams(
    fan_min_speed=10,
    fan_control_type=FAN_CONTROL_DC,
    fan_max_temp=45,
    fan_off_delta=2,
    fan_start_temp=35,
    fan_mode=FAN_MODE_CONTINUOUS,
    fan_monitoring_enabled=True,
    fan2_monitoring_enabled=True,
    fan3_monitoring_enabled=True,
    fan4_monitoring_enabled=True,
    overheat_alarm_enabled=True,
)



class OutputLimitState(IntEnum):
    NORMAL = 0
    MEASURE = 1
    VERIFY_FORMULA = 2
    VERIFY_STEP = 3
    VERIFY_MINIMUM = 4
    LIMITED = 5
    MINIMUM = 6
    SENSOR_ERROR = 7
    FAULT = 8
    CONFIRMING = 9
    START_BLOCKED = 10
    VERIFY_TIMEOUT = 11


class OutputLimitResetResult(IntEnum):
    NONE = 0
    PENDING = 1
    APPLIED = 2
    REJECTED = 3


@dataclass(frozen=True)
class OutputLimitStatus:
    configured: int
    effective: int
    state: OutputLimitState
    reset_sequence: Optional[int] = None
    reset_result: OutputLimitResetResult = OutputLimitResetResult.NONE
    temporary_minimum: Optional[int] = None
    alert_without_load: bool = False



class ThermalInputState(IntEnum):
    READY = 0
    ADC_INITIALIZING = 1
    ADC_OFFLINE = 2
    NTC_UNAVAILABLE = 3


@dataclass(frozen=True)
class FanModeStatus:
    selected: int
    effective: int
    thermal_input: ThermalInputState


class ProfileSkipReason(IntEnum):
    NONE = 0
    INVALID = 1
    OUTPUT_MAX = 2
    TEMPERATURE_CONFLICT = 3
    ADC_UNAVAILABLE = 4
    NTC_UNAVAILABLE = 5
    OUTPUT_ADJUSTING = 6


@dataclass(frozen=True)
class ProfileApplyResult:
    slot: int
    status: int
    applied_mask: int
    skipped: tuple[ProfileSkipReason, ...]

    @property
    def ok(self) -> bool:
        return self.status == 0
