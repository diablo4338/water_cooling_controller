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
    PROFILE_ACTION_APPLY,
    PROFILE_ACTION_CLEAR,
    PROFILE_COUNT,
    PROFILE_ACTION_SAVE_CURRENT,
    PROFILE_DESCRIPTION_MAX_LEN,
    PROFILE_METADATA_VERSION,
    PROFILE_STATUS_VERSION,
    PROFILE_TITLE_MAX_LEN,
)

PARAMS_FORMAT_V5 = "<BHiBiiiBBBBB"
PARAMS_FORMAT = "<BHiBiiiBBBBBB"
PARAMS_PAYLOAD_LEN_V5 = struct.calcsize(PARAMS_FORMAT_V5)
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


def encode_params(params: DeviceParams, mask: int = 0x07FF) -> bytes:
    if params.protocol_version == 5:
        return struct.pack(
            PARAMS_FORMAT_V5,
            5,
            int(mask) & 0x03FF,
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
        int(bool(params.overheat_alarm_enabled)),
    )


def decode_params(data: bytes) -> DeviceParams:
    if len(data) == PARAMS_PAYLOAD_LEN_V5:
        values = struct.unpack(PARAMS_FORMAT_V5, data)
        if values[0] != 5:
            raise RuntimeError(f"Unsupported params version={values[0]}")
        (
            version, mask, fan_min_speed, fan_control_type, fan_max_temp,
            fan_off_delta, fan_start_temp, fan_mode, fan_monitoring_enabled,
            fan2_monitoring_enabled, fan3_monitoring_enabled, fan4_monitoring_enabled,
        ) = values
        overheat_alarm_enabled = True
    elif len(data) == PARAMS_PAYLOAD_LEN:
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
            overheat_alarm_enabled,
        ) = struct.unpack(PARAMS_FORMAT, data)
        if version != PARAMS_VERSION:
            raise RuntimeError(f"Unsupported params version={version}")
    else:
        raise RuntimeError(f"Bad params len={len(data)}")
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
        overheat_alarm_enabled=bool(overheat_alarm_enabled),
        protocol_version=int(version),
    )


def decode_params_status(data: bytes) -> ParamsStatus:
    if len(data) != 3:
        raise RuntimeError(f"Bad params status len={len(data)}")
    version, status, field_id = struct.unpack("<BBB", data)
    if version not in (5, PARAMS_VERSION):
        raise RuntimeError(f"Unsupported params status version={version}")
    field = None if field_id == PARAM_FIELD_NONE else int(field_id)
    return ParamsStatus(ok=status == PARAM_STATUS_OK, status=status, field_id=field)


def encode_profile_command(action: int, slot: int) -> bytes:
    if action not in (
        PROFILE_ACTION_SAVE_CURRENT,
        PROFILE_ACTION_APPLY,
        PROFILE_ACTION_CLEAR,
    ):
        raise ValueError(f"Unknown profile action: {action}")
    if slot < 1 or slot > PROFILE_COUNT:
        raise ValueError(f"Profile slot must be between 1 and {PROFILE_COUNT}")
    return struct.pack("<BB", action, slot)


def decode_profile_slots(data: bytes) -> frozenset[int]:
    if len(data) != 2:
        raise RuntimeError(f"Bad profile status len={len(data)}")
    version, mask = struct.unpack("<BB", data)
    if version != PROFILE_STATUS_VERSION:
        raise RuntimeError(f"Unsupported profile status version={version}")
    if mask & ~((1 << PROFILE_COUNT) - 1):
        raise RuntimeError(f"Invalid profile mask={mask}")
    return frozenset(slot for slot in range(1, PROFILE_COUNT + 1) if mask & (1 << (slot - 1)))


def encode_profile_metadata(title: str, description: str) -> bytes:
    title_bytes = title.strip().encode("utf-8")
    description_bytes = description.strip().encode("utf-8")
    if not title_bytes or len(title_bytes) > PROFILE_TITLE_MAX_LEN:
        raise ValueError(f"Profile title must be 1-{PROFILE_TITLE_MAX_LEN} UTF-8 bytes")
    if len(description_bytes) > PROFILE_DESCRIPTION_MAX_LEN:
        raise ValueError(
            f"Profile description must be at most {PROFILE_DESCRIPTION_MAX_LEN} UTF-8 bytes"
        )
    return struct.pack(
        "<BBB", PROFILE_METADATA_VERSION, len(title_bytes), len(description_bytes)
    ) + title_bytes + description_bytes


def decode_profile_metadata(data: bytes) -> dict[str, str]:
    if len(data) < 3:
        raise RuntimeError(f"Bad profile metadata len={len(data)}")
    version, title_len, description_len = struct.unpack("<BBB", data[:3])
    if version != PROFILE_METADATA_VERSION:
        raise RuntimeError(f"Unsupported profile metadata version={version}")
    if (
        title_len > PROFILE_TITLE_MAX_LEN
        or description_len > PROFILE_DESCRIPTION_MAX_LEN
        or len(data) != 3 + title_len + description_len
    ):
        raise RuntimeError("Invalid profile metadata lengths")
    try:
        title = data[3:3 + title_len].decode("utf-8")
        description = data[3 + title_len:].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise RuntimeError("Profile metadata is not valid UTF-8") from exc
    return {"title": title, "description": description}


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
