#!/usr/bin/env python3
import argparse
import asyncio
import binascii
import os
from typing import Optional

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.serialization import (
    Encoding,
    NoEncryption,
    PrivateFormat,
    PublicFormat,
    load_pem_private_key,
)

UUID_PAIR_SVC = "8fdd08d6-2a9e-4d5a-9f44-9f58b3a9d3c1"
UUID_MAIN_SVC = "3d1a4b35-9707-43e6-bf3e-2e2f7b561d82"

UUID_PAIR_DEV_NONCE = "0b46b3cf-7e3b-44a3-8f39-4af2a8c9a1ee"
UUID_PAIR_DEV_PUB = "91c66f66-5c92-4c4d-86bf-6d2c58b6f0d7"
UUID_PAIR_HOST_PUB = "c9c8f69a-1f49-4ea0-a0a2-3c0d0a69e9d4"
UUID_PAIR_CONFIRM = "f5ee9c0b-96ae-4dc0-9b46-5f6f7f2ad2bf"
UUID_PAIR_FINISH = "a4c8e2c1-1c7b-4b06-a59f-4b5f8a2a8b3c"

UUID_AUTH_NONCE = "f1d1f9b6-8c92-47f6-a2f5-5b0a77d2e3a9"
UUID_AUTH_PROOF = "74cde77a-7f14-4e6e-b7f5-92ef0c3ad7e4"

UUID_MAIN_DATA = "a9b66c3d-3a6e-4b75-8b67-1dfbdb2a7e11"

PAIR_SVC_NORM = UUID_PAIR_SVC.replace("-", "").lower()

HOST_KEY_PATH = os.path.join(os.path.dirname(__file__), "..", "host_key.pem")


def log(msg: str) -> None:
    print(msg, flush=True)


def hmac_sha256(key: bytes, msg: bytes) -> bytes:
    h = hmac.HMAC(key, hashes.SHA256())
    h.update(msg)
    return h.finalize()


def derive_k(shared_secret: bytes, dev_nonce: bytes) -> bytes:
    hkdf = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=dev_nonce,
        info=b"PAIRv1",
    )
    return hkdf.derive(shared_secret)


def load_or_create_host_key() -> ec.EllipticCurvePrivateKey:
    if os.path.exists(HOST_KEY_PATH):
        with open(HOST_KEY_PATH, "rb") as f:
            return load_pem_private_key(f.read(), password=None)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption())
    with open(HOST_KEY_PATH, "wb") as f:
        f.write(pem)
    return key


async def scan_for_pairing_device(timeout_s: float) -> Optional[str]:
    found = None

    def on_detect(device, advertisement_data) -> None:
        nonlocal found
        if found:
            return
        uuids = advertisement_data.service_uuids or []
        for raw in uuids:
            norm = raw.replace("-", "").lower()
            if norm == PAIR_SVC_NORM or norm.endswith(PAIR_SVC_NORM):
                found = device.address
                return

    async with BleakScanner(detection_callback=on_detect):
        await asyncio.sleep(timeout_s)

    return found


async def expect_error(label: str, op) -> None:
    try:
        await op()
    except Exception as exc:
        log(f"PASS (expected error): {label} -> {exc}")
        return
    raise RuntimeError(f"FAIL (expected error, got success): {label}")


async def expect_ok(label: str, op) -> None:
    try:
        await op()
    except Exception as exc:
        raise RuntimeError(f"FAIL (unexpected error): {label} -> {exc}")
    log(f"PASS: {label}")


async def integration_test(address: Optional[str], scan_timeout: float) -> None:
    host_key = load_or_create_host_key()

    if not address:
        log("Scanning for pairing advertisement... Press pair button now.")
        address = await scan_for_pairing_device(scan_timeout)
        if not address:
            raise RuntimeError("No pairing device found. Ensure pairing mode is ON.")

    log(f"Using device address: {address}")

    k = None

    async with BleakClient(address) as client:
        log("Connected (pairing mode). Testing ordering guards...")

        await expect_error(
            "PAIR_FINISH before HOST_PUB",
            lambda: client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=True),
        )
        await expect_error(
            "PAIR_CONFIRM before HOST_PUB",
            lambda: client.write_gatt_char(UUID_PAIR_CONFIRM, b"\x00" * 32, response=True),
        )

        dev_nonce = await client.read_gatt_char(UUID_PAIR_DEV_NONCE)
        dev_pub65 = await client.read_gatt_char(UUID_PAIR_DEV_PUB)
        if len(dev_nonce) != 16:
            raise RuntimeError(f"Bad dev_nonce len={len(dev_nonce)}")
        if len(dev_pub65) != 65 or dev_pub65[0] != 0x04:
            raise RuntimeError("Bad dev_pub format (expected 65 bytes uncompressed P-256)")

        host_pub65 = host_key.public_key().public_bytes(
            Encoding.X962, PublicFormat.UncompressedPoint
        )

        dev_pub_obj = ec.EllipticCurvePublicKey.from_encoded_point(
            ec.SECP256R1(), bytes(dev_pub65)
        )
        shared = host_key.exchange(ec.ECDH(), dev_pub_obj)
        k = derive_k(shared, bytes(dev_nonce))

        await expect_ok(
            "PAIR_HOST_PUB",
            lambda: client.write_gatt_char(UUID_PAIR_HOST_PUB, host_pub65, response=True),
        )
        confirm = hmac_sha256(k, b"confirm" + bytes(dev_nonce))
        await expect_ok(
            "PAIR_CONFIRM",
            lambda: client.write_gatt_char(UUID_PAIR_CONFIRM, confirm, response=True),
        )
        await expect_ok(
            "PAIR_FINISH",
            lambda: client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=False),
        )

    log("Reconnecting in main mode...")
    async with BleakClient(address) as client:
        await expect_error(
            "READ MAIN_DATA without auth",
            lambda: client.read_gatt_char(UUID_MAIN_DATA),
        )

        nonce = await client.read_gatt_char(UUID_AUTH_NONCE)
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await client.disconnect()

    log("Reconnecting to test stale auth nonce...")
    async with BleakClient(address) as client:
        await expect_error(
            "AUTH_PROOF with stale nonce",
            lambda: client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True),
        )

        nonce = await client.read_gatt_char(UUID_AUTH_NONCE)
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await expect_ok(
            "AUTH_PROOF",
            lambda: client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True),
        )

        data = await client.read_gatt_char(UUID_MAIN_DATA)
        log(f"PASS: READ MAIN_DATA after auth ({binascii.hexlify(data).decode()[:16]}...)")

        got_notify = asyncio.Event()

        def on_notify(_: int, payload: bytearray) -> None:
            log(f"NOTIFY MAIN_DATA ({binascii.hexlify(payload).decode()[:16]}...)")
            got_notify.set()

        await client.start_notify(UUID_MAIN_DATA, on_notify)
        try:
            await asyncio.wait_for(got_notify.wait(), timeout=3.0)
            log("PASS: NOTIFY received")
        except asyncio.TimeoutError:
            raise RuntimeError("FAIL: expected notify but none received")
        finally:
            await client.stop_notify(UUID_MAIN_DATA)

        await expect_error(
            "READ PAIR_DEV_NONCE after pairing complete",
            lambda: client.read_gatt_char(UUID_PAIR_DEV_NONCE),
        )

    log("Reconnecting to verify auth works without re-pairing...")
    async with BleakClient(address) as client:
        nonce = await client.read_gatt_char(UUID_AUTH_NONCE)
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await expect_ok(
            "AUTH_PROOF after reconnect",
            lambda: client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True),
        )


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="BLE integration tests")
    p.add_argument("--address", help="Device BLE address (skip scan)")
    p.add_argument("--scan-timeout", type=float, default=8.0)
    return p.parse_args()


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(integration_test(args.address, args.scan_timeout))
    except Exception as exc:
        log(str(exc))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
