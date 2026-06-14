# PCWCC

A desktop application for controlling the device over BLE. The GUI is built with `PySide6`, and the BLE layer uses `bleak`. The application runs on Linux and Windows; Linux releases are packaged as `AppImage`.

## Features
- scanning for the controller in pairing mode;
- pairing and subsequent application-level authorization;
- connecting to the device and reading temperatures in real time;
- reading and applying cooling parameters;
- BLE session diagnostics through debug mode.

Application-local data:
- `paired_devices.json` - trusted device database;
- `host_key.pem` - host private key used for pairing.

## Structure
- `src/pcwcc/core_logic/` - BLE protocol, codecs, pairing, control, and storage.
- `src/pcwcc/gui_logic/` - UI logic.
- `src/pcwcc/main.py` - entry point.
- `tests/` - unit and integration tests for the client.
- `packaging/` - desktop packaging assets.
- `pcwcc.spec` - `PyInstaller` configuration.

## Run
From the `pcwcc/` directory:

```bash
PYTHONPATH=src python -m pcwcc.main
```

From the repository root:

```bash
make run-app
```

For extended BLE diagnostics:

```bash
PYTHONPATH=src python -m pcwcc.main --debug
```

## Tests
Unit tests:

```bash
PYTHONPATH=src python -m pytest -c pytest.ini -q -m "not integration"
```

Integration tests from the repository root:

```bash
PYTHONPATH=pcwcc/src python -m pytest -c pcwcc/pytest.ini -q -m integration
```

Test dependencies are listed in `tests/requirements.txt`.

## Environment variables
Application:
- `BLE_SCAN_TIMEOUT_S`
- `BLE_RESOLVE_TIMEOUT_S`
- `BLE_CONNECT_TIMEOUT_S`
- `BLE_PAIR_TIMEOUT_S`
- `BLE_AUTH_TIMEOUT_S`
- `BLE_METRICS_TIMEOUT_S`
- `BLE_METRICS_RETRIES`
- `BLE_METRICS_RECONNECT_DELAY_S`
- `BLE_RESOLVE_BEFORE_CONNECT`
- `BLE_USE_SERVICE_FILTER`
- `BLE_WINRT_USE_CACHED_SERVICES`
- `BLE_WINRT_ADDRESS_TYPE`
- `GUI_ACTION_DEFAULT_TIMEOUT_S`
- `GUI_ACTION_SCAN_TIMEOUT_S`
- `GUI_ACTION_PAIR_TIMEOUT_S`
- `GUI_ACTION_CONNECT_TIMEOUT_S`
- `GUI_ACTION_DISCONNECT_TIMEOUT_S`

Integration tests:
- `BLE_ADDRESS`
- `BLE_ADAPTER`
- `SCAN_TIMEOUT_S`
- `CONNECT_TIMEOUT_S`
- `PRESS_BASE_URL`
- `PRESS_TIMEOUT_S`
- `PRESS_RETRIES`
- `PRESS_NO_RESPONSE`

## Packaging
The Linux `AppImage` is built by [`../scripts/build_appimage.sh`](../scripts/build_appimage.sh). Details are in [`packaging/appimage/README.md`](packaging/appimage/README.md).
