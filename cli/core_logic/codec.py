from __future__ import annotations

import binascii
import struct

from .models import DeviceParams, DeviceStatus, FanStatus, OperationStatus, ParamsStatus
from .protocol import (
    DEVICE_ERROR_KNOWN_MASK,
    DEVICE_ERROR_NAMES,
    DEVICE_ERROR_NONE,
    DEVICE_STATE_NAMES,
    DEVICE_STATUS_VERSION,
    FAN_STATE_NAMES,
    FAN_STATUS_VERSION,
    OP_CONTROL_VERSION,
    OP_ERROR_TEXT_MAX,
    OP_STATUS_VERSION,
    OP_TYPE_NAMES,
    PARAM_FIELD_NONE,
    PARAM_STATUS_OK,
    PARAMS_VERSION,
)

PARAMS_FORMAT = "<BHiBiiiBBBBB"
PARAMS_PAYLOAD_LEN = struct.calcsize(PARAMS_FORMAT)


def decode_metrics(data: bytes) -> str:
    value = decode_temp_value(data)
    if value is None:
        return "0x" + binascii.hexlify(data).decode()
    return f"{value:.2f}"


def decode_temp_value(data: bytes) -> float | None:
    if len(data) != 4:
        return None
    return struct.unpack("<f", data)[0]


def decode_fan_speed(data: bytes) -> float | None:
    return decode_temp_value(data)


def decode_power_metric(data: bytes) -> float | None:
    return decode_temp_value(data)


def encode_params(params: DeviceParams, mask: int = 0x03FF) -> bytes:
    return struct.pack(
        PARAMS_FORMAT,
        PARAMS_VERSION,
        int(mask),
        int(params.fan_min_speed),
        int(params.fan_control_type),
        int(params.fan_max_temp),
        int(params.fan_off_delta),
        int(params.fan_start_temp),
        int(params.fan_mode),
        int(bool(params.fan_monitoring_enabled)),
        int(bool(params.fan2_monitoring_enabled)),
        int(bool(params.fan3_monitoring_enabled)),
        int(bool(params.fan4_monitoring_enabled)),
    )


def decode_params(data: bytes) -> DeviceParams:
    if len(data) != PARAMS_PAYLOAD_LEN:
        raise RuntimeError(f"Bad params len={len(data)}")
    (
        version,
        mask,
        fan_min_speed,
        fan_control_type,
        fan_max_temp,
        fan_off_delta,
        fan_start_temp,
        fan_mode,
        fan_monitoring_enabled,
        fan2_monitoring_enabled,
        fan3_monitoring_enabled,
        fan4_monitoring_enabled,
    ) = struct.unpack(PARAMS_FORMAT, data)
    if version != PARAMS_VERSION:
        raise RuntimeError(f"Unsupported params version={version}")
    _ = mask
    return DeviceParams(
        fan_min_speed=int(fan_min_speed),
        fan_control_type=int(fan_control_type),
        fan_max_temp=int(fan_max_temp),
        fan_off_delta=int(fan_off_delta),
        fan_start_temp=int(fan_start_temp),
        fan_mode=int(fan_mode),
        fan_monitoring_enabled=bool(fan_monitoring_enabled),
        fan2_monitoring_enabled=bool(fan2_monitoring_enabled),
        fan3_monitoring_enabled=bool(fan3_monitoring_enabled),
        fan4_monitoring_enabled=bool(fan4_monitoring_enabled),
    )


def decode_params_status(data: bytes) -> ParamsStatus:
    if len(data) != 3:
        raise RuntimeError(f"Bad params status len={len(data)}")
    version, status, field_id = struct.unpack("<BBB", data)
    if version != PARAMS_VERSION:
        raise RuntimeError(f"Unsupported params status version={version}")
    field = None if field_id == PARAM_FIELD_NONE else int(field_id)
    return ParamsStatus(ok=status == PARAM_STATUS_OK, status=status, field_id=field)


def decode_fan_status(data: bytes) -> FanStatus:
    if len(data) != 6:
        raise RuntimeError(f"Bad fan status len={len(data)}")
    version, s1, s2, s3, s4, op_type = struct.unpack("<BBBBBB", data)
    if version != FAN_STATUS_VERSION:
        raise RuntimeError(f"Unsupported fan status version={version}")
    states = (int(s1), int(s2), int(s3), int(s4))
    labels = tuple(FAN_STATE_NAMES.get(state, "UNKNOWN") for state in states)
    op_label = OP_TYPE_NAMES.get(op_type, "UNKNOWN")
    return FanStatus(states=states, labels=labels, op_type=int(op_type), op_label=op_label)


def _decode_device_error_mask(error_mask: int) -> tuple[tuple[int, ...], tuple[str, ...]]:
    if error_mask == DEVICE_ERROR_NONE:
        return (), ()
    errors = tuple(
        flag for flag in sorted(DEVICE_ERROR_NAMES) if flag != DEVICE_ERROR_NONE and (error_mask & flag)
    )
    labels = tuple(DEVICE_ERROR_NAMES.get(flag, f"UNKNOWN_0x{flag:08X}") for flag in errors)
    unknown_bits = error_mask & ~DEVICE_ERROR_KNOWN_MASK
    if unknown_bits:
        bit = 1
        while bit <= unknown_bits:
            if unknown_bits & bit:
                errors += (bit,)
                labels += (f"UNKNOWN_0x{bit:08X}",)
            bit <<= 1
    return errors, labels


def decode_device_status(data: bytes) -> DeviceStatus:
    if len(data) != 6:
        raise RuntimeError(f"Bad device status len={len(data)}")
    version, state, error_mask = struct.unpack("<BBI", data)
    if version != DEVICE_STATUS_VERSION:
        raise RuntimeError(f"Unsupported device status version={version}")
    errors, error_labels = _decode_device_error_mask(int(error_mask))
    return DeviceStatus(
        state=int(state),
        label=DEVICE_STATE_NAMES.get(state, "UNKNOWN"),
        error_mask=int(error_mask),
        errors=errors,
        error_labels=error_labels,
    )


def encode_operation_control(op_type: int, action: int = 1) -> bytes:
    return struct.pack("<BBB", OP_CONTROL_VERSION, op_type, action)


def decode_operation_status(data: bytes) -> OperationStatus:
    expected_len = 4 + OP_ERROR_TEXT_MAX
    if len(data) != expected_len:
        raise RuntimeError(f"Bad operation status len={len(data)}")
    version, op_type, state, err_len = struct.unpack("<BBBB", data[:4])
    if version != OP_STATUS_VERSION:
        raise RuntimeError(f"Unsupported operation status version={version}")
    err_len = min(int(err_len), OP_ERROR_TEXT_MAX)
    err_raw = data[4:4 + OP_ERROR_TEXT_MAX]
    err_text = ""
    if err_len > 0:
        err_text = err_raw[:err_len].decode("utf-8", errors="replace")
    return OperationStatus(op_type=int(op_type), state=int(state), error=err_text)
