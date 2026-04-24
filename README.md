# PC Water Cooling Controller

This repository contains a GUI application and firmware for a BLE-based controller that manages a PC water-cooling loop. The app can pair with the device, connect to it, stream temperature metrics in real time, and apply cooling parameters.

The GUI is built with `PySide6`, the BLE layer uses `bleak`. Pairing and authorization are implemented at the application level over GATT. This is not OS-level bonding.

Local data:
- `paired_devices.json` - trusted devices database (address, name, k_hex, last_connected).
- `host_key.pem` - host private key (P-256) used for pairing.

## Quick start
1. Install GUI dependencies from `ble_app/requirements.txt`.
2. Run the app: `python -m ble_app.main` or `make run-app`.

Buttons service (for integration tests):
- `make run-buttons` or `uvicorn raspberry.app:app --host 0.0.0.0 --port 8001`.

Firmware (ESP-IDF):
- `make fw` - build/flash.
- `make fw-tests` - build/flash with `PAIR_RUN_TESTS=1`.

## Directory structure
- `ble_app/` - GUI app and the shared BLE layer used by the GUI and tests.
- `mk/` - ESP-IDF firmware (NimBLE) with protocol implementation and metrics.
- `raspberry/` - HTTP service for the GPIO button, used by integration tests.
- `paired_devices.json` - trusted devices database.
- `host_key.pem` - host private key.
- `ARCHITECTURE.md` - architecture details and module relationships.

## Pairing protocol
Scanning only shows devices that advertise `PAIR_SVC`. This indicates readiness to pair. In pairing mode the device advertises the name `sensor-pair`; in normal mode it advertises `sensor`.
Pairing mode is a separate 60-second window used only to add trusted clients. It is enabled by the hardware button, ends on timeout or reboot, and does not wipe previously saved clients.

### Pairing service `PAIR_SVC`
Characteristics:
- `PAIR_DEV_NONCE` (read): `nonce_d` (16 bytes)
- `PAIR_DEV_PUB` (read): `d_pub` (65 bytes, P-256 uncompressed)
- `PAIR_HOST_PUB` (write): `h_pub` (65 bytes, P-256 uncompressed)
- `PAIR_CONFIRM` (write): `HMAC(K, b"confirm"+nonce_d)`
- `PAIR_FINISH` (write): `0x01` - finish pairing and store trust

Algorithm:
1. The host connects to the device in pairing mode.
2. Reads `PAIR_DEV_NONCE` and `PAIR_DEV_PUB`.
3. Computes ECDH `shared`, then `K = HKDF(shared, salt=nonce_d, info="PAIRv1", len=32)`.
4. Writes `PAIR_HOST_PUB`, then `PAIR_CONFIRM`, then `PAIR_FINISH`.
5. The device stores the host identity (hash of `h_pub`) and derived key in the trusted clients store.

Trusted clients:
- Up to 5 clients are stored on the device.
- Re-pairing the same host updates its existing slot.
- Adding a 6th distinct host overwrites the oldest slot in round-robin order.

## Other services (brief)
After pairing, the device operates in normal mode. Authorization and access to metrics use `MAIN_SVC`, cooling parameters use `CONFIG_SVC`, maintenance operations use `OPERATIONS_SVC`, and metrics use `METRICS_SVC`.

### Main service `MAIN_SVC`
Characteristics:
- `AUTH_NONCE` (read): 16 bytes
- `AUTH_PROOF` (write): `HMAC(K, b"auth"+nonce)`

AUTH algorithm:
1. The host connects to the device (normal mode).
2. Reads `AUTH_NONCE`.
3. Writes `AUTH_PROOF`.
4. After successful auth, the device allows reading/notify of metrics.

### Config service `CONFIG_SVC`
Characteristics:
- `CONFIG_PARAMS` (read/write): parameter payload (version, mask, 3 float32 LE)
- `CONFIG_STATUS` (read/notify/write): apply status; write `0x01` to apply
- `CONFIG_FAN_STATUS` (read/notify): fan status (`IDLE/STARTING/RUNNING/STALL/IN_SERVICE`)
- `CONFIG_DEVICE_STATUS` (read/notify): global device status (`OK/ERROR`)

Payload `CONFIG_PARAMS`:
- `version` (uint8) = `1`
- `mask` (uint8): bit0=target_temp_c, bit1=fan_min_rpm, bit2=alarm_delta_c
- `target_temp_c` (float32 LE)
- `fan_min_rpm` (float32 LE)
- `alarm_delta_c` (float32 LE)

Payload `CONFIG_STATUS`:
- `version` (uint8) = `1`
- `status` (uint8): `0=OK`, `1=INVALID`, `2=BUSY`
- `field` (uint8): `0..2` or `0xFF` for generic error

Payload `CONFIG_FAN_STATUS`:
- `version` (uint8) = `1`
- `state` (uint8): `0=IDLE`, `1=STARTING`, `2=RUNNING`, `3=STALL`, `4=IN_SERVICE`
- `op_type` (uint8): `0=NONE`, `1=FAN_CALIBRATION` (set when `state=IN_SERVICE`)

Payload `CONFIG_DEVICE_STATUS`:
- `version` (uint8) = `2`
- `state` (uint8): `0=OK`, `1=ERROR`
- `error_mask` (uint32 LE): bitmask of active errors
- bit0: `ADC_OFFLINE`
- bit1: `NTC_DISCONNECTED`

Parameters are cached in RAM and persisted to NVS; firmware loads them from NVS on boot.

### Operations service `OPERATIONS_SVC`
Characteristics:
- `OP_CONTROL` (write): start operation (`version/op_type/action`)
- `OP_STATUS` (read/notify): operation state

Payload `OP_CONTROL`:
- `version` (uint8) = `1`
- `op_type` (uint8): `1=FAN_CALIBRATION`
- `action` (uint8): `1=start`

Payload `OP_STATUS` (fixed length 24 bytes):
- `version` (uint8) = `1`
- `op_type` (uint8): `1=FAN_CALIBRATION`
- `state` (uint8): `0=IDLE`, `1=IN_SERVICE`, `2=DONE`, `3=ERROR`
- `err_len` (uint8): `0..20`
- `err_text` (char[20], ASCII/UTF-8), only set when `state=ERROR`
If an operation is already active at startup, the device responds with `state=ERROR` and `err_text="busy"`.

### Metrics service `METRICS_SVC`
Characteristics:
- `TEMP0_VALUE` (read/notify): float32 LE
- `TEMP1_VALUE` (read/notify): float32 LE
- `TEMP2_VALUE` (read/notify): float32 LE
- `TEMP3_VALUE` (read/notify): float32 LE

## UUID (must match firmware)
PAIR_SVC: `8fdd08d6-2a9e-4d5a-9f44-9f58b3a9d3c1`
MAIN_SVC: `3d1a4b35-9707-43e6-bf3e-2e2f7b561d82`
METRICS_SVC: `f3a0c1d2-5b6a-4d2e-9b43-1c2d3e4f5061`
CONFIG_SVC: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e10`
OPERATIONS_SVC: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e20`

PAIR_DEV_NONCE: `0b46b3cf-7e3b-44a3-8f39-4af2a8c9a1ee`
PAIR_DEV_PUB: `91c66f66-5c92-4c4d-86bf-6d2c58b6f0d7`
PAIR_HOST_PUB: `c9c8f69a-1f49-4ea0-a0a2-3c0d0a69e9d4`
PAIR_CONFIRM: `f5ee9c0b-96ae-4dc0-9b46-5f6f7f2ad2bf`
PAIR_FINISH: `a4c8e2c1-1c7b-4b06-a59f-4b5f8a2a8b3c`

AUTH_NONCE: `f1d1f9b6-8c92-47f6-a2f5-5b0a77d2e3a9`
AUTH_PROOF: `74cde77a-7f14-4e6e-b7f5-92ef0c3ad7e4`

CONFIG_PARAMS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e11`
CONFIG_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e12`
CONFIG_FAN_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e13`
CONFIG_DEVICE_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e14`
OP_CONTROL: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e21`
OP_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e22`

TEMP0_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718291`
TEMP1_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718292`
TEMP2_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718293`
TEMP3_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718294`

## Tests
Integration tests use only the public API from `ble_app`. Unit tests for UI logic live in `ble_app/tests/test_presentation.py` and do not require PySide6.

Test dependencies: `ble_app/tests/requirements.txt`.

Run:
```bash
pytest -c ble_app/pytest.ini -q -m integration
```

## Environment variables
Application (GUI/ble_app):
- `BLE_SCAN_TIMEOUT_S` - scan timeout.
- `BLE_RESOLVE_TIMEOUT_S` - address resolve timeout.
- `BLE_CONNECT_TIMEOUT_S` - connect timeout.
- `BLE_PAIR_TIMEOUT_S` - pairing timeout.
- `BLE_AUTH_TIMEOUT_S` - auth timeout.
- `BLE_METRICS_TIMEOUT_S` - metrics read timeout.
- `BLE_METRICS_RETRIES` - metrics read retries.
- `BLE_METRICS_RECONNECT_DELAY_S` - reconnect delay between metrics reads.
- `GUI_ACTION_DEFAULT_TIMEOUT_S` - default UI action timeout.
- `GUI_ACTION_SCAN_TIMEOUT_S` - UI scan timeout.
- `GUI_ACTION_PAIR_TIMEOUT_S` - UI pairing timeout.
- `GUI_ACTION_CONNECT_TIMEOUT_S` - UI connect timeout.
- `GUI_ACTION_DISCONNECT_TIMEOUT_S` - UI disconnect timeout.

Integration tests:
- `BLE_ADDRESS` - device address to skip scanning.
- `BLE_ADAPTER` - BLE adapter (for example, `hci0`).
- `SCAN_TIMEOUT_S` - scan timeout (tests).
- `CONNECT_TIMEOUT_S` - connect timeout (tests).
- `PRESS_BASE_URL` - base URL for the button service.
- `PRESS_TIMEOUT_S` - HTTP press timeout.
- `PRESS_RETRIES` - number of retries.
- `PRESS_NO_RESPONSE` - do not wait for HTTP response (1/0).
