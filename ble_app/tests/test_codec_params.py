import struct

from ble_app.core_logic.codec import PARAMS_PAYLOAD_LEN, decode_device_status, decode_params, encode_params
from ble_app.core_logic.models import DeviceParams
from ble_app.core_logic.protocol import DEVICE_ERROR_OVERHEAT, DEVICE_STATE_ERROR


def test_params_round_trip_uses_full_payload_length() -> None:
    params = DeviceParams(
        fan_min_speed=42,
        fan_control_type=1,
        fan_max_temp=65,
        fan_off_delta=7,
        fan_start_temp=33,
        fan_mode=2,
        fan_monitoring_enabled=True,
        fan2_monitoring_enabled=False,
        fan3_monitoring_enabled=True,
        fan4_monitoring_enabled=False,
    )

    payload = encode_params(params)

    assert len(payload) == PARAMS_PAYLOAD_LEN
    assert decode_params(payload) == params


def test_decode_device_status_exposes_overheat_flag() -> None:
    payload = struct.pack("<BBI", 2, DEVICE_STATE_ERROR, DEVICE_ERROR_OVERHEAT)

    status = decode_device_status(payload)

    assert status.error_mask == DEVICE_ERROR_OVERHEAT
    assert status.errors == (DEVICE_ERROR_OVERHEAT,)
    assert status.error_labels == ("OVERHEAT",)
