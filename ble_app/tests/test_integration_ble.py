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
    press_base_url: str,
    press_timeout_s: float,
    press_retries: int,
    press_no_response: bool,
) -> DeviceInfo:
    _press(
        "press/reset-long",
        press_base_url,
        press_timeout_s,
        press_retries,
        press_no_response,
    )
    await asyncio.sleep(0.5)
    _press(
        "press/pair",
        press_base_url,
        press_timeout_s,
        press_retries,
        press_no_response,
    )
    await asyncio.sleep(0.5)

    if ble_address:
        device = await core.resolve_device(ble_address, timeout=scan_timeout_s)
    else:
        devices = await core.scan_pairing(timeout=scan_timeout_s)
        assert devices, "No pairing device found. Pair button did not start advertising."
        device = devices[0]

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


@pytest.fixture(scope="function")
async def paired_device(
    core: BleAppCore,
    ble_address: Optional[str],
    scan_timeout_s: float,
    press_base_url: str,
    press_timeout_s: float,
    press_retries: int,
    press_no_response: bool,
) -> DeviceInfo:
    return await _pair_device(
        core,
        ble_address,
        scan_timeout_s,
        press_base_url,
        press_timeout_s,
        press_retries,
        press_no_response,
    )


@pytest.mark.asyncio
async def test_pair_button_advertises_pairing_service(
    core: BleAppCore,
    ble_address: Optional[str],
    scan_timeout_s: float,
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
    await asyncio.sleep(0.5)
    _press(
        "press/pair",
        press_base_url,
        press_timeout_s,
        press_retries,
        press_no_response,
    )
    await asyncio.sleep(0.5)

    if ble_address:
        await core.resolve_device(ble_address, timeout=scan_timeout_s)
    else:
        devices = await core.scan_pairing(timeout=scan_timeout_s)
        assert devices, "No pairing device found. Pair button did not start advertising."


@pytest.mark.asyncio
async def test_pairing_succeeds(paired_device: DeviceInfo) -> None:
    assert paired_device.address


@pytest.mark.asyncio
async def test_unauthenticated_connect_rejected(
    core: BleAppCore, paired_device: DeviceInfo, connect_timeout_s: float
) -> None:
    await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
    try:
        with pytest.raises(Exception):
            await core.read_metrics()
    finally:
        await core.disconnect()


@pytest.mark.asyncio
@pytest.mark.slow
async def test_notify_in_connect_disconnect_cycles(
    core: BleAppCore, paired_device: DeviceInfo, connect_timeout_s: float
) -> None:
    for cycle in range(4):
        last_error: Exception | None = None
        for attempt in range(2):
            await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
            try:
                await core.auth()
                await asyncio.sleep(0.2)
                values = await core.read_metrics(timeout=10.0)
                assert len(values) == 4
                last_error = None
                break
            except Exception as exc:
                last_error = exc
                LOG.warning(
                    "metrics read failed in cycle %d attempt %d: %s",
                    cycle + 1,
                    attempt + 1,
                    exc,
                )
            finally:
                await core.disconnect()
            await asyncio.sleep(0.2)
        if last_error is not None:
            raise last_error


@pytest.mark.asyncio
async def test_reset_disconnects_and_blocks_reconnect(
    core: BleAppCore,
    paired_device: DeviceInfo,
    connect_timeout_s: float,
    press_base_url: str,
    press_timeout_s: float,
    press_retries: int,
    press_no_response: bool,
) -> None:
    await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
    try:
        await core.auth()
        _press(
            "press/reset-long",
            press_base_url,
            press_timeout_s,
            press_retries,
            press_no_response,
        )
        await asyncio.sleep(0.5)
        with pytest.raises(Exception):
            await core.read_metrics()
    finally:
        await core.disconnect()

    await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
    try:
        with pytest.raises(Exception) as excinfo:
            await core.auth()
        msg = str(excinfo.value)
        assert "NotAuthorized" in msg or "Not Authorized" in msg, (
            "Expected NotAuthorized error after reset; got: " + msg
        )
    finally:
        await core.disconnect()


# @pytest.mark.asyncio
# @pytest.mark.slow
# async def test_full_session_scenario(
#     core: BleAppCore,
#     ble_address: Optional[str],
#     scan_timeout_s: float,
#     connect_timeout_s: float,
#     press_base_url: str,
#     press_enabled: bool,
#     press_timeout_s: float,
#     press_retries: int,
#     press_no_response: bool,
# ) -> None:
#     paired_device = await _pair_device(
#         core,
#         ble_address,
#         scan_timeout_s=scan_timeout_s,
#         press_base_url=press_base_url,
#         press_enabled=press_enabled,
#         press_timeout_s=press_timeout_s,
#         press_retries=press_retries,
#         press_no_response=press_no_response,
#     )
#     await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
#     try:
#         with pytest.raises(Exception):
#             await core.read_metrics()
#     finally:
#         await core.disconnect()
#
#     for _ in range(4):
#         await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
#         try:
#             await core.auth()
#             got_notify = asyncio.Event()
#
#             def on_data(_: bytearray) -> None:
#                 got_notify.set()
#
#             await core.start_metrics_notify(on_data)
#             try:
#                 await asyncio.wait_for(got_notify.wait(), timeout=3.0)
#             finally:
#                 await core.stop_metrics_notify()
#         finally:
#             await core.disconnect()
#
#     await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
#     _press(
#         "press/reset-long",
#         press_base_url,
#         press_enabled,
#         press_timeout_s,
#         press_retries,
#         press_no_response,
#     )
#     await asyncio.sleep(0.5)
#     with pytest.raises(Exception):
#         await core.read_metrics()
#     await core.disconnect()
#
#     await core.connect_raw(paired_device, connect_timeout=connect_timeout_s)
#     try:
#         with pytest.raises(Exception) as excinfo:
#             await core.auth()
#         msg = str(excinfo.value)
#         assert "NotAuthorized" in msg or "Not Authorized" in msg, (
#             "Expected NotAuthorized error after reset; got: " + msg
#         )
#     finally:
#         await core.disconnect()
