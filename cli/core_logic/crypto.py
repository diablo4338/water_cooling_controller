from __future__ import annotations

from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.serialization import (
    Encoding,
    NoEncryption,
    PrivateFormat,
    load_pem_private_key,
)

from .storage import HOST_KEY_PATH


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
    try:
        with open(HOST_KEY_PATH, "rb") as handle:
            return load_pem_private_key(handle.read(), password=None)
    except FileNotFoundError:
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption())
        with open(HOST_KEY_PATH, "wb") as handle:
            handle.write(pem)
        return key
