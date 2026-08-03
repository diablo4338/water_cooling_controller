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

The application requires Python 3.11, 3.12, or 3.13. Python 3.14 is not
currently supported. Runtime and development dependencies are declared in
[`pyproject.toml`](pyproject.toml); the project does not use a separate
`requirements.txt`.

### Windows (PowerShell)

From the repository root, create a virtual environment using Python 3.13:

```powershell
py -3.13 -m venv .venv
```

Install the application and its development dependencies:

```powershell
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -e ".\pcwcc[dev]"
```

Run the application:

```powershell
.\.venv\Scripts\python.exe -m pcwcc.main
```

Run it with extended BLE diagnostics:

```powershell
.\.venv\Scripts\python.exe -m pcwcc.main --debug
```

The editable installation (`-e`) means source code changes are picked up when
the application is restarted; reinstalling the package after each change is
not required.

If `py -3.13` reports that no suitable Python installation was found, install
64-bit Python 3.13 first, then repeat the commands above.

### Linux

From the `pcwcc/` directory, after installing the project dependencies:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -e ".[dev]"
PYTHONPATH=src python -m pcwcc.main
```

Alternatively, from the repository root:

```bash
make run-app
```

`make run-app` installs the application and its development dependencies from
`pcwcc/pyproject.toml` before starting it. To install without starting the GUI,
use:

```bash
make install-app
```

For extended BLE diagnostics:

```bash
make run-app APP_ARGS=--debug
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

### Windows executable

From the repository root, after installing the development dependencies, build
the Windows application with PyInstaller:

```powershell
.\.venv\Scripts\python.exe -m PyInstaller `
  --name PCWCC `
  --windowed `
  --onedir `
  --paths ".\pcwcc\src" `
  ".\pcwcc\src\pcwcc\main.py"
```

The executable is written to `dist\PCWCC\PCWCC.exe`. Windows builds disable
filtered and cached WinRT GATT service discovery by default to avoid
`ERROR_BAD_COMMAND (0x80070016)` with the controller. The behaviour can still
be overridden with `BLE_USE_SERVICE_FILTER` and
`BLE_WINRT_USE_CACHED_SERVICES`.
