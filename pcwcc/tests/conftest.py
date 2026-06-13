import os
import sys
from typing import Optional

import pytest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
APP_SRC_DIR = os.path.join(PROJECT_ROOT, "pcwcc", "src")
for path in (APP_SRC_DIR, PROJECT_ROOT):
    if path not in sys.path:
        sys.path.insert(0, path)


def _str_to_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "y", "on"}


def pytest_sessionstart(session: pytest.Session) -> None:
    config = session.config
    run_integration = bool(config.getoption("--runintegration"))
    run_manual = bool(config.getoption("--runmanual"))
    ble_address_env = os.environ.get("BLE_ADDRESS", "").strip()
    if (run_integration or run_manual) and not ble_address_env:
        raise pytest.UsageError(
            "BLE_ADDRESS env var must be set and non-empty to run integration/manual tests."
        )


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--ble-address",
        default=os.environ.get("BLE_ADDRESS"),
        help="BLE device address (env: BLE_ADDRESS)",
    )
    parser.addoption(
        "--ble-adapter",
        default=os.environ.get("BLE_ADAPTER"),
        help="BLE adapter name (env: BLE_ADAPTER), e.g. hci0",
    )
    parser.addoption(
        "--scan-timeout",
        default=os.environ.get("SCAN_TIMEOUT_S", "8.0"),
        help="Scan timeout seconds (env: SCAN_TIMEOUT_S)",
    )
    parser.addoption(
        "--connect-timeout",
        default=os.environ.get("CONNECT_TIMEOUT_S", "20.0"),
        help="BLE connect timeout seconds (env: CONNECT_TIMEOUT_S)",
    )
    parser.addoption(
        "--press-base-url",
        default=os.environ.get("PRESS_BASE_URL", "http://192.168.1.60:8001"),
        help="Base URL for button presses (env: PRESS_BASE_URL)",
    )
    parser.addoption(
        "--press-timeout",
        default=os.environ.get("PRESS_TIMEOUT_S", "10.0"),
        help="HTTP press timeout seconds (env: PRESS_TIMEOUT_S)",
    )
    parser.addoption(
        "--press-retries",
        default=os.environ.get("PRESS_RETRIES", "2"),
        help="HTTP press retries (env: PRESS_RETRIES)",
    )
    parser.addoption(
        "--press-no-response",
        default=os.environ.get("PRESS_NO_RESPONSE", "0"),
        help="Do not wait for HTTP response body (1/0, env: PRESS_NO_RESPONSE)",
    )
    parser.addoption(
        "--runslow",
        action="store_true",
        default=False,
        help="Run tests marked as slow",
    )
    parser.addoption(
        "--runintegration",
        action="store_true",
        default=False,
        help="Run tests marked as integration",
    )
    parser.addoption(
        "--runmanual",
        action="store_true",
        default=False,
        help="Run tests marked as manual",
    )


@pytest.fixture(scope="session")
def ble_address(pytestconfig: pytest.Config) -> Optional[str]:
    return pytestconfig.getoption("--ble-address")


@pytest.fixture(scope="session")
def ble_adapter(pytestconfig: pytest.Config) -> Optional[str]:
    return pytestconfig.getoption("--ble-adapter")


@pytest.fixture(scope="session")
def scan_timeout_s(pytestconfig: pytest.Config) -> float:
    return float(pytestconfig.getoption("--scan-timeout"))


@pytest.fixture(scope="session")
def connect_timeout_s(pytestconfig: pytest.Config) -> float:
    return float(pytestconfig.getoption("--connect-timeout"))


@pytest.fixture(scope="session")
def press_base_url(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("--press-base-url"))


@pytest.fixture(scope="session")
def press_enabled(pytestconfig: pytest.Config) -> bool:
    return _str_to_bool(str(pytestconfig.getoption("--press-enabled")))


@pytest.fixture(scope="session")
def press_timeout_s(pytestconfig: pytest.Config) -> float:
    return float(pytestconfig.getoption("--press-timeout"))


@pytest.fixture(scope="session")
def press_retries(pytestconfig: pytest.Config) -> int:
    return int(pytestconfig.getoption("--press-retries"))


@pytest.fixture(scope="session")
def press_no_response(pytestconfig: pytest.Config) -> bool:
    return _str_to_bool(str(pytestconfig.getoption("--press-no-response")))


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    run_slow = bool(config.getoption("--runslow"))
    run_integration = bool(config.getoption("--runintegration"))
    run_manual = bool(config.getoption("--runmanual"))

    skip_slow = pytest.mark.skip(reason="need --runslow option to run")
    skip_integration = pytest.mark.skip(reason="need --runintegration option to run")
    skip_manual = pytest.mark.skip(reason="need --runmanual option to run")

    for item in items:
        if "slow" in item.keywords and not run_slow:
            item.add_marker(skip_slow)
        if "integration" in item.keywords and not run_integration:
            item.add_marker(skip_integration)
        if "manual" in item.keywords and not run_manual:
            item.add_marker(skip_manual)
