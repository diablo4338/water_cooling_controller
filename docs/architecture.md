# Architecture

The project is a PC water cooling controller. The system consists of a GUI app (BLE client), device firmware, and a small HTTP service used by integration tests.

| Reference | Coverage |
| --- | --- |
| [Runtime behavior](runtime.md) | Fan modes, protection, service operations, configuration, and BLE delivery |
| [State machines and event routing](state-machines.md) | Owners, inputs, transition guards, emitted effects, and recipients |
| [Settings reference](settings.md) | Persistent parameters, firmware constants, wire formats, and client environment variables |
| [Design milestones](design-milestones.md) | Decisions behind the current runtime and earlier algorithm choices |
| [Firmware diagnostics](firmware.md) | Serial logs and client chart history |

Key artifacts:
- `paired_devices.json` - trusted devices database on the host.
- `host_key.pem` - persistent host private key (P-256) used for pairing.

## App (pcwcc)
- `pcwcc/src/pcwcc/config.py` - loads configuration from env, shared timeouts/retries for BLE and GUI.
- `pcwcc/src/pcwcc/core.py` - public BLE API and protocol (pair/auth/metrics/config/operations). Used by GUI and tests.
- `pcwcc/src/pcwcc/presentation.py` - pure UI state logic (Actions, AppState, derive_ui).
- `pcwcc/src/pcwcc/gui_logic/` - PySide6 views and the `BleWorker` that calls `BleAppCore`.
- `pcwcc/src/pcwcc/main.py` - minimal GUI entrypoint.
- `pcwcc/tests/` - unit and integration tests, run through `pcwcc/src/pcwcc/core.py`.

## Firmware (firmware/)
- `firmware/src/ble_protocol/app/main.c` - `app_main`, NVS, NimBLE, timers, metrics init.
- `firmware/src/ble_protocol/ble/gap.c` - advertising, connect/disconnect, PAIR/MAIN mode selection.
- `firmware/src/ble_protocol/ble/gatt.c` - GATT database (PAIR/MAIN/METRICS), read/write callbacks.
- `firmware/src/ble_protocol/ble/uuid.c` - UUID strings, firmware source of truth.
- `firmware/src/ble_protocol/pairing/state.c` - main access state machine (trusted/unauth/authed) and session handles.
- `firmware/src/ble_protocol/pairing/pair_state.c` - pairing flow steps.
- `firmware/src/ble_protocol/pairing/pair_mode.c` - separate 60-second pairing window and per-session ECDH material.
- `firmware/src/ble_protocol/pairing/conn_guard.c` - connection handle constraints.
- `firmware/src/ble_protocol/pairing/crypto.c`, `ecdh.c`, `host_verify.c` - cryptography and host identity checks.
- `firmware/src/ble_protocol/pairing/storage.c` - ring-buffer storage for up to 5 trusted host/key entries in NVS.
- `firmware/src/ble_protocol/ble/metrics_ble.c` - notify/handle metrics over BLE.
- `firmware/src/ble_protocol/control/` - fan regulation, temperature and current protection, service operations, and configuration.
- `firmware/src/metrics/` - ADS1115 temperatures, INA226 voltage and current, and fan tachometers.
- `firmware/tests/` - firmware test sources compiled when `PAIR_RUN_TESTS=1`.
- `firmware/build/` - ESP-IDF build artifacts.

## Support service (tests/raspberry/)
- `tests/raspberry/app.py` - HTTP endpoint for the Raspberry Pi GPIO button, used by integration tests.

## BLE protocol surface
GATT services are split by responsibility:
- `PAIR_SVC` - pairing and initial key exchange.
- `MAIN_SVC` - authorization before data access.
- `METRICS_SVC` - temperatures, tachometer RPM, voltage, current, and applied output snapshots and notifications.
- `CONFIG_SVC` - cooling system parameters and status.
- `OPERATIONS_SVC` - maintenance operations (for example, fan calibration).

UUID constants must be synchronized between firmware (`firmware/src/ble_protocol/ble/uuid.c`) and the app (`pcwcc/src/pcwcc/core_logic/protocol.py`).

## Public API Contract
The client exposes BLE behavior through `pcwcc/src/pcwcc/core.py`. Wire constants and codecs live under `core_logic/`, and firmware handlers live in `ble/gatt.c`.
Actions from `pcwcc/src/pcwcc/presentation.py` are the public contract between the GUI and UI logic. Renaming Actions requires updating UI bindings and tests.
All interactive GUI elements are tagged with `actionId = Action.name` and used for smoke checks.
