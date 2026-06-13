from __future__ import annotations

import asyncio
import binascii
from typing import Optional

from bleak import BleakScanner
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from .crypto import derive_k, hmac_sha256
from .models import DeviceInfo, PairResult
from .protocol import (
    PAIR_SVC_NORM,
    PAIRING_SERVICE_UUIDS,
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
        self._emit_conn(f"scan pairing: start (timeout={timeout:.1f}s)")
        candidates: dict[str, DeviceInfo] = {}

        def on_detect(device, advertisement_data) -> None:
            uuids = advertisement_data.service_uuids or []
            norm_uuids = {normalize_uuid(raw) for raw in uuids}
            name = advertisement_data.local_name or device.name or "Unknown"
            if PAIR_SVC_NORM in norm_uuids:
                candidates[device.address] = DeviceInfo(
                    name=name,
                    address=device.address,
                    ble_device=device,
                )
                self._emit_conn(
                    f"scan pairing: candidate {name} ({device.address}), uuids={len(uuids)}"
                )

        async with BleakScanner(detection_callback=on_detect, **self._scanner_kwargs()):
            await asyncio.sleep(timeout)
        self._emit_conn(f"scan pairing: probing {len(candidates)} candidate(s)")

        results: dict[str, DeviceInfo] = {}
        for device in candidates.values():
            if await self._probe_pairing(device):
                results[device.address] = device
        self._emit_conn(f"scan pairing: ready devices={len(results)}")
        return list(results.values())

    async def _probe_pairing(self, device: DeviceInfo) -> bool:
        client = None
        try:
            target = await self._resolve_ble_target(device, timeout=self._config.resolve_timeout_s)
            client = self._make_client(
                target,
                connect_timeout=self._config.connect_timeout_s,
                services=PAIRING_SERVICE_UUIDS,
            )
            await self._run_step(
                f"probe connect {self._format_device(device)}",
                client.connect(),
                timeout=self._config.connect_timeout_s,
            )
            await self._run_step(
                f"probe read {self._uuid_label(UUID_PAIR_DEV_NONCE)}",
                client.read_gatt_char(UUID_PAIR_DEV_NONCE),
                timeout=self._config.pair_timeout_s,
            )
            self._emit_conn(f"probe pairing {self._format_device(device)}: ready")
            return True
        except Exception as exc:
            self._emit_conn(
                f"probe pairing {self._format_device(device)}: rejected: {self._format_error(exc)}"
            )
            return False
        finally:
            if client is not None:
                try:
                    await self._run_step(
                        f"probe disconnect {self._format_device(device)}",
                        client.disconnect(),
                        timeout=3.0,
                    )
                except Exception:
                    pass

    async def resolve_device(self, address: str, timeout: Optional[float] = None) -> DeviceInfo:
        if timeout is None:
            timeout = self._config.resolve_timeout_s
        self._emit_conn(f"resolve by address {address}: start (timeout={timeout:.1f}s)")
        dev = await self._run_step(
            f"resolve by address {address}",
            BleakScanner.find_device_by_address(address, timeout=timeout, **self._scanner_kwargs()),
            timeout=timeout + 0.5,
        )
        if not dev:
            raise RuntimeError(f"Device with address {address} was not found.")
        device = DeviceInfo(name=dev.name or "Unknown", address=dev.address, ble_device=dev)
        self._emit_conn(f"resolve by address {address}: found {self._format_device(device)}")
        return device

    async def pair(self, device: DeviceInfo, timeout: Optional[float] = None) -> PairResult:
        if timeout is None:
            timeout = self._config.pair_timeout_s
        k_hex: Optional[str] = None
        self._emit_conn(
            f"pair {self._format_device(device)}: start (timeout={timeout:.1f}s)"
        )
        client = None
        try:
            target = await self._resolve_ble_target(device, timeout=self._config.resolve_timeout_s)
            client = self._make_client(
                target,
                connect_timeout=self._config.connect_timeout_s,
                services=PAIRING_SERVICE_UUIDS,
            )
            await self._run_step(
                f"pair connect {self._format_device(device)}",
                client.connect(),
                timeout=self._config.connect_timeout_s,
            )
            try:
                dev_nonce = await self._run_step(
                    f"pair read {self._uuid_label(UUID_PAIR_DEV_NONCE)}",
                    client.read_gatt_char(UUID_PAIR_DEV_NONCE),
                    timeout=timeout,
                )
                dev_pub65 = await self._run_step(
                    f"pair read {self._uuid_label(UUID_PAIR_DEV_PUB)}",
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
                self._emit_conn(
                    f"pair {self._format_device(device)}: derived shared key, k_len={len(k)}"
                )

                await self._run_step(
                    f"pair write {self._uuid_label(UUID_PAIR_HOST_PUB)}",
                    client.write_gatt_char(UUID_PAIR_HOST_PUB, host_pub65, response=True),
                    timeout=timeout,
                )
                confirm = hmac_sha256(k, b"confirm" + bytes(dev_nonce))
                await self._run_step(
                    f"pair write {self._uuid_label(UUID_PAIR_CONFIRM)}",
                    client.write_gatt_char(UUID_PAIR_CONFIRM, confirm, response=True),
                    timeout=timeout,
                )
                await self._run_step(
                    f"pair write {self._uuid_label(UUID_PAIR_FINISH)}",
                    client.write_gatt_char(UUID_PAIR_FINISH, b"\x01", response=True),
                    timeout=timeout,
                )
                self._emit_conn(f"pair {self._format_device(device)}: completed")
            finally:
                if client is not None:
                    try:
                        await self._run_step(
                            f"pair disconnect {self._format_device(device)}",
                            client.disconnect(),
                            timeout=3.0,
                        )
                    except Exception:
                        pass
        except Exception as exc:
            self._emit_conn(
                f"pair {self._format_device(device)}: failed: {self._format_error(exc)}"
            )
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
        self._emit_conn(
            f"auth {self._format_device(self.device)}: start (timeout={timeout:.1f}s)"
        )
        nonce = await self._read_char(
            UUID_AUTH_NONCE,
            timeout=timeout,
            label=f"auth read {self._uuid_label(UUID_AUTH_NONCE)}",
        )
        if len(nonce) != 16:
            raise RuntimeError(f"Bad auth nonce len={len(nonce)}")
        proof = hmac_sha256(k, b"auth" + bytes(nonce))
        await self._write_char(
            UUID_AUTH_PROOF,
            proof,
            response=True,
            timeout=timeout,
            label=f"auth write {self._uuid_label(UUID_AUTH_PROOF)}",
        )
        self._emit_conn(f"auth {self._format_device(self.device)}: completed")
