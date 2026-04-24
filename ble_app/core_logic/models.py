from __future__ import annotations

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


@dataclass(frozen=True)
class MetricsSnapshot:
    temperatures: tuple[Optional[float], Optional[float], Optional[float], Optional[float]]
    fan_speeds: tuple[Optional[float], Optional[float], Optional[float], Optional[float]]

    def with_temperature(self, channel: int, value: Optional[float]) -> "MetricsSnapshot":
        if channel < 0 or channel >= len(self.temperatures):
            return self
        temperatures = list(self.temperatures)
        temperatures[channel] = value
        return MetricsSnapshot(tuple(temperatures), self.fan_speeds)

    def with_fan_speed(self, channel: int, value: Optional[float]) -> "MetricsSnapshot":
        if channel < 0 or channel >= len(self.fan_speeds):
            return self
        fan_speeds = list(self.fan_speeds)
        fan_speeds[channel] = value
        return MetricsSnapshot(self.temperatures, tuple(fan_speeds))

    @classmethod
    def empty(cls) -> "MetricsSnapshot":
        return cls(
            temperatures=(None, None, None, None),
            fan_speeds=(None, None, None, None),
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
)
