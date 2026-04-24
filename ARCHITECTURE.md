# Architecture

The project is a PC water cooling controller. The system consists of a GUI app (BLE client), device firmware, and a small HTTP service used by integration tests.

Key artifacts:
- `paired_devices.json` - trusted devices database on the host.
- `host_key.pem` - persistent host private key (P-256) used for pairing.

## App (ble_app)
- `ble_app/config.py` - loads configuration from env, shared timeouts/retries for BLE and GUI.
- `ble_app/core.py` - public BLE API and protocol (pair/auth/metrics/config/operations). Used by GUI and tests.
- `ble_app/presentation.py` - pure UI state logic (Actions, AppState, derive_ui).
- `ble_app/gui.py` - PySide6 GUI and `BleWorker` that calls `BleAppCore`.
- `ble_app/main.py` - minimal GUI entrypoint.
- `ble_app/tests/` - unit and integration tests, run through `ble_app/core.py`.

## Firmware (mk/)
- `mk/ble_protocol/main.c` - `app_main`, NVS, NimBLE, timers, metrics init.
- `mk/ble_protocol/gap.c` - advertising, connect/disconnect, PAIR/MAIN mode selection.
- `mk/ble_protocol/gatt.c` - GATT database (PAIR/MAIN/METRICS), read/write callbacks.
- `mk/ble_protocol/uuid.c` - UUID strings, firmware source of truth.
- `mk/ble_protocol/state.c` - main access state machine (trusted/unauth/authed) and session handles.
- `mk/ble_protocol/pair_state.c` - pairing flow steps.
- `mk/ble_protocol/pair_mode.c` - separate 60-second pairing window and per-session ECDH material.
- `mk/ble_protocol/conn_guard.c` - connection handle constraints.
- `mk/ble_protocol/crypto.c`, `ecdh.c`, `host_verify.c` - cryptography and host identity checks.
- `mk/ble_protocol/storage.c` - ring-buffer storage for up to 5 trusted host/key entries in NVS.
- `mk/ble_protocol/metrics_ble.c` - notify/handle metrics over BLE.
- `mk/metrics/` - metrics source (ADS1115 and aggregation).
- `mk/build/` - ESP-IDF build artifacts.

## Support service (raspberry/)
- `raspberry/app.py` - HTTP endpoint for the GPIO button, used by integration tests.

## BLE protocol surface
GATT services are split by responsibility:
- `PAIR_SVC` - pairing and initial key exchange.
- `MAIN_SVC` - authorization before data access.
- `METRICS_SVC` - temperature metrics (notify).
- `CONFIG_SVC` - cooling system parameters and status.
- `OPERATIONS_SVC` - maintenance operations (for example, fan calibration).

UUID constants must be synchronized between firmware (`mk/ble_protocol/uuid.c`) and the app (`ble_app/core.py`).

## Public API Contract
Any changes in `ble_app/core.py` must be reflected in tests and the GUI. This file is the source of truth for BLE behavior.
UUID constants in `ble_app/core.py` must match `mk/ble_protocol/uuid.c`.
Actions from `ble_app/presentation.py` are the public contract between the GUI and UI logic. Renaming Actions requires updating UI bindings and tests.
All interactive GUI elements are tagged with `actionId = Action.name` and used for smoke checks.
