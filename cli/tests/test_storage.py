from pathlib import Path

from cli.core import load_paired_records, rename_paired_record, save_paired_records
from cli.core_logic import storage


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
