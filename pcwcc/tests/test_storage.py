from pathlib import Path

from pcwcc.core import load_paired_records, rename_paired_record, save_paired_records
from pcwcc.core_logic import DeviceParams, storage


def test_rename_paired_record_updates_name(tmp_path: Path, monkeypatch) -> None:
    db_path = tmp_path / "paired_devices.json"
    monkeypatch.setattr(storage, "PAIRED_DB", str(db_path))
    save_paired_records(
        [
            {"name": "Old name", "address": "AA:BB", "k_hex": "01", "last_connected": 0},
            {"name": "Other", "address": "CC:DD", "k_hex": "02", "last_connected": 0},
        ]
    )

    assert rename_paired_record("AA:BB", "New name") is True

    assert load_paired_records() == [
        {"name": "New name", "address": "AA:BB", "k_hex": "01", "last_connected": 0},
        {"name": "Other", "address": "CC:DD", "k_hex": "02", "last_connected": 0},
    ]


def test_rename_paired_record_rejects_blank_name(tmp_path: Path, monkeypatch) -> None:
    db_path = tmp_path / "paired_devices.json"
    monkeypatch.setattr(storage, "PAIRED_DB", str(db_path))
    save_paired_records(
        [{"name": "Old name", "address": "AA:BB", "k_hex": "01", "last_connected": 0}]
    )

    assert rename_paired_record("AA:BB", "   ") is False
    assert load_paired_records() == [
        {"name": "Old name", "address": "AA:BB", "k_hex": "01", "last_connected": 0}
    ]


def test_device_profiles_round_trip_and_are_isolated_by_device(
    tmp_path: Path, monkeypatch
) -> None:
    profiles_path = tmp_path / "profiles.json"
    monkeypatch.setattr(storage, "PROFILES_DB", str(profiles_path))
    params = DeviceParams(
        fan_min_speed=35,
        fan_control_type=1,
        fan_max_temp=55,
        fan_off_delta=3,
        fan_start_temp=40,
        fan_mode=2,
        fan_monitoring_enabled=True,
        fan2_monitoring_enabled=False,
        fan3_monitoring_enabled=True,
        fan4_monitoring_enabled=False,
        overheat_alarm_enabled=True,
        protocol_version=6,
    )

    storage.save_device_profile("AA:BB", 2, params)

    assert storage.load_device_profiles("AA:BB") == {2: params}
    assert storage.load_device_profiles("CC:DD") == {}


def test_save_device_profile_rejects_invalid_slot(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(storage, "PROFILES_DB", str(tmp_path / "profiles.json"))

    try:
        storage.save_device_profile("AA:BB", 0, storage.DEFAULT_PARAMS)
    except ValueError as exc:
        assert "between 1 and 5" in str(exc)
    else:
        raise AssertionError("invalid profile slot was accepted")


def test_profile_metadata_round_trip_and_clear_keeps_custom_label(
    tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setattr(storage, "PROFILES_DB", str(tmp_path / "profiles.json"))
    params = storage.DEFAULT_PARAMS
    storage.save_device_profile("AA:BB", 1, params)
    storage.save_device_profile_metadata("AA:BB", 1, "Quiet", "Low noise setup")

    assert storage.load_device_profile_metadata("AA:BB") == {
        1: {"title": "Quiet", "description": "Low noise setup"}
    }
    assert storage.clear_device_profile("AA:BB", 1) is True
    assert storage.load_device_profiles("AA:BB") == {}
    assert storage.load_device_profile_metadata("AA:BB") == {
        1: {"title": "Quiet", "description": "Low noise setup"}
    }
    assert storage.clear_device_profile("AA:BB", 1) is False
