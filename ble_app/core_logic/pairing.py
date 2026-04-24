from __future__ import annotations

import asyncio
import binascii
from typing import Optional

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from .crypto import derive_k, hmac_sha256
from .models import DeviceInfo, PairResult
from .protocol import (
    PAIR_SVC_NORM,
    UUID_AUTH_NONCE,
    UUID_AUTH_PROOF,
    UUID_PAIR_CONFIRM,
    UUID_PAIR_DEV_NONCE,
    UUID_PAIR_DEV_PUB,
    UUID_PAIR_FINISH,
    UUID_PAIR_HOST_PUB,
    normalize_uuid,
)
from .storage import find_paired_record


class BleCorePairingMixin:
    async def scan_pairing(self, timeout: Optional[float] = None) -> list[DeviceInfo]:
        if timeout is None:
            timeout = self._config.scan_timeout_s
        candidates: dict[str, DeviceInfo] = {}

        def on_detect(device, advertisement_data) -> None:
            uuids = advertisement_data.service_uuids or []
            norm_uuids = {normalize_uuid(raw) for raw in uuids}
            name = advertisement_data.local_name or device.name or "Unknown"
            if PAIR_SVC_NORM in norm_uuids:
                candidates[device.address] = DeviceInfo(name=name, address=device.address)

        async with BleakScanner(detection_callback=on_detect, **self._scanner_kwargs()):
            await asyncio.sleep(timeout)

        results: dict[str, DeviceInfo] = {}
        for device in candidates.values():
            if await self._probe_pairing(device):
                results[device.address] = device
        return list(results.values())

    async def _probe_pairing(self, device: DeviceInfo) -> bool:
        client = BleakClient(device.address, adapter=self._adapter)
        try:
            await asyncio.wait_for(client.connect(), timeout=self._config.connect_timeout_s)
            await asyncio.wait_for(
                client.read_gatt_char(UUID_PAIR_DEV_NONCE),
                timeout=self._config.pair_timeout_s,
            )
            return True
        except Exception:
            return False
        finally:
            try:
                await client.disconnect()
            except Exception:
                pass

    async def resolve_device(self, address: str, timeout: Optional[float] = None) -> DeviceInfo:
        if timeout is None:
            timeout = self._config.resolve_timeout_s
        dev = await BleakScanner.find_device_by_address(
            address, timeout=timeout, **self._scanner_kwargs()
        )
        if not dev:
            raise RuntimeError(f"Device with address {address} was not found.")
        return DeviceInfo(name=dev.name or "Unknown", address=dev.address)

    async def pair(self, device: DeviceInfo, timeout: Optional[float] = None) -> PairResult:
        if timeout is None:
            timeout = self._config.pair_timeout_s
        k_hex: Optional[str] = None
        client = BleakClient(device.address, adapter=self._adapter)
        try:
            async with client:
                dev_nonce = await asyncio.wait_for(
                    client.read_gatt_char(UUID_PAIR_DEV_NONCE),
                    timeout=timeout,
                )
                dev_pub65 = await asyncio.wait_for(
                    client.read_gatt_char(UUID_PAIR_DEV_PUB),
                    timeout=timeout,
                )
                if len(dev_nonce) != 16:
                    raise RuntimeError(f"Bad dev_nonce len={len(dev_nonce)}")
                if len(dev_pub65) != 65 or dev_pub65[0] != 0x04:
                    raise RuntimeError("Bad dev_pub format (expected 65 bytes uncompressed P-256)")

                host_pub65 = self._host_key.public_key().public_bytes(
                    Encoding.X962,
                    PublicFormat.UncompressedPoint,
                )
                dev_pub_obj = ec.EllipticCurvePublicKey.from_encoded_point(
                    ec.SECP256R1(),
                    bytes(dev_pub65),
                )
                shared = self._host_key.exchange(ec.ECDH(), dev_pub_obj)
                k = derive_k(shared, bytes(dev_nonce))
                k_hex = binascii.hexlify(k).decode()

                await asyncio.wait_for(
                    client.write_gatt_char(UUID_PAIR_HOST_PUB, host_pub65, response=True),
                    timeout=timeout,
                )
                confirm = hmac_sha256(k, b"confirm" + bytes(dev_nonce))
                await asyncio.wait_for(
                    client.write_gatt_char(UUID_PAIR_CONFIRM, confirm, response=True),
                    timeout=timeout,
                )
                await asyncio.wait_for(
                    client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=True),
                    timeout=timeout,
                )
        except Exception as exc:
            return PairResult(False, f"Pairing failed: {exc}", None)
        return PairResult(True, "Paired", k_hex)

    async def auth(self, timeout: Optional[float] = None) -> None:
        if timeout is None:
            timeout = self._config.auth_timeout_s
        if not self.client or not self.device:
            raise RuntimeError("Not connected")
        record = find_paired_record(self.device.address)
        if not record:
            raise RuntimeError("Device not paired")
        if "k_hex" not in record:
            raise RuntimeError("Missing K")
        k = binascii.unhexlify(record["k_hex"])
        nonce = await asyncio.wait_for(self.client.read_gatt_char(UUID_AUTH_NONCE), timeout=timeout)
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await asyncio.wait_for(
            self.client.write_gatt_char(UUID_AUTH_PROOF, proof, response=True),
            timeout=timeout,
        )
