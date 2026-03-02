import asyncio
import logging
import time
from typing import Optional

import pytest

from ble_app import (
    BleAppCore,
    DeviceInfo,
    add_or_update_paired,
)

LOG = logging.getLogger(__name__)
pytestmark = pytest.mark.integration


def _press(
        path: str,
        base_url: str,
        timeout_s: float,
        retries: int,
        no_response: bool,
        require_status_lt_400: bool = False,
) -> None:
    import socket
    from urllib.parse import urlparse

    base = base_url.rstrip("/")
    url = f"{base}/{path.lstrip('/')}"
    parsed = urlparse(url)
    host = parsed.hostname
    if not host:
        raise RuntimeError("URL has no hostname")
    port = parsed.port or 80
    request = f"GET {parsed.path or '/'} HTTP/1.0\r\n\r\n".encode("ascii")

    last_error: Exception | None = None
    for attempt in range(retries + 1):
        try:
            LOG.info("Pressing via %s (attempt %d)", url, attempt + 1)
            with socket.create_connection((host, port), timeout=timeout_s) as sock:
                sock.settimeout(timeout_s)
                sock.sendall(request)
                if no_response:
                    return
                data = b""
                while b"\r\n" not in data:
                    chunk = sock.recv(256)
                    if not chunk:
                        break
                    data += chunk
                line = data.split(b"\r\n", 1)[0].decode("iso-8859-1", errors="replace")
                if not line.startswith("HTTP/"):
                    raise RuntimeError(f"Invalid HTTP response line: {line!r}")
                parts = line.split()
                if len(parts) < 2:
                    raise RuntimeError(f"Invalid HTTP response line: {line!r}")
                try:
                    status = int(parts[1])
                except ValueError as exc:
                    raise RuntimeError(
                        f"Invalid HTTP status code in response line: {line!r}"
                    ) from exc
                if require_status_lt_400 and status >= 400:
                    raise RuntimeError(f"HTTP status {status} from {url}")
            return
        except Exception as exc:
            last_error = exc
            LOG.warning("Press failed: %s", exc)
            if attempt < retries:
                time.sleep(0.3)
    raise RuntimeError(
        f"Press failed after {retries + 1} attempts: {url}. "
    ) from last_error


async def _pair_device(
        core: BleAppCore,
        ble_address: Optional[str],
        scan_timeout_s: float,
) -> DeviceInfo:
    device = await core.resolve_device(ble_address, timeout=scan_timeout_s)
    last_msg = ""
    for attempt in range(3):
        pair_result = await core.pair(device)
        if pair_result.ok and pair_result.k_hex:
            add_or_update_paired(device, pair_result.k_hex)
            return device
        last_msg = pair_result.message
        if "InProgress" in last_msg:
            await asyncio.sleep(0.8)
            continue
        break
    raise AssertionError(f"Pairing failed: {last_msg}")


@pytest.fixture(scope="session", autouse=True)
def _configure_logging() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


@pytest.fixture(scope="session")
def core(ble_adapter: Optional[str]) -> BleAppCore:
    return BleAppCore(log=LOG.info, adapter=ble_adapter)


@pytest.fixture(scope="function", autouse=True)
def _auto_reset_pairing(
        press_base_url: str,
        press_timeout_s: float,
        press_retries: int,
        press_no_response: bool,
) -> None:
    _press(
        "press/reset-long",
        press_base_url,
        press_timeout_s,
        press_retries,
        press_no_response,
    )
    time.sleep(0.5)


@pytest.fixture(scope="function")
def paired_device(
        core: BleAppCore,
        ble_address: Optional[str],
        scan_timeout_s: float,
        press_base_url: str,
        press_timeout_s: float,
        press_retries: int,
        press_no_response: bool,
) -> DeviceInfo:
    return asyncio.run(
        _pair_device(
            core,
            ble_address,
            scan_timeout_s
        )
    )


@pytest.mark.asyncio
async def test_pair_button_advertises_pairing_service(
        core: BleAppCore,
        ble_address: Optional[str],
) -> None:
    assert await core._probe_pairing(DeviceInfo(name="Test", address=ble_address))


@pytest.mark.asyncio
async def test_pairing_succeeds(paired_device: DeviceInfo) -> None:
    assert paired_device.address


@pytest.mark.asyncio
async def test_unauthorize_read_metrics(core: BleAppCore, ble_address: Optional[str]) -> None:
    await core.connect_raw(DeviceInfo(name="Test", address=ble_address))
    with pytest.raises(RuntimeError):
        await core.read_metrics(timeout=10)
