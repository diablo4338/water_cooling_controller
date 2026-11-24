from pathlib import Path
import time

import pytest

from pcwcc.core import load_paired_records, rename_paired_record, save_paired_records
from pcwcc.core_logic import storage


def test_metrics_history_round_trips_legacy_and_additional_channels(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(storage, "METRICS_HISTORY_DIR", str(tmp_path))
    now = time.time()
    legacy = {"timestamp": now - 10, "temp3_c": 30, "temp4_c": None, "fan_percent": 50}
    current = {
        "timestamp": now,
        "temp1_c": 22,
        "temp2_c": 28,
        "fan1_rpm": 1200,
        "fan2_rpm": 0,
        "fan3_rpm": 700,
        "fan4_rpm": 850,
    }
    storage.save_metrics_history("AA:BB", [legacy, current], now=now)

    assert storage.load_metrics_history("AA:BB", now=now) == [legacy, current]
    assert storage.load_metrics_history_all("AA:BB") == [legacy, current]


def test_rename_paired_record_updates_name(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
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


def test_rename_paired_record_rejects_blank_name(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    db_path = tmp_path / "paired_devices.json"
    monkeypatch.setattr(storage, "PAIRED_DB", str(db_path))
    save_paired_records(
        [{"name": "Old name", "address": "AA:BB", "k_hex": "01", "last_connected": 0}]
    )

    assert rename_paired_record("AA:BB", "   ") is False
    assert load_paired_records() == [
        {"name": "Old name", "address": "AA:BB", "k_hex": "01", "last_connected": 0}
    ]
