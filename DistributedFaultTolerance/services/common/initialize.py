"""Runs once before the nodes start, as root (the setup container in docker-compose.yml).

- Makes the RSA key pair that signs session tokens if there isn't one yet. The private key goes
  into a volume only the auth node mounts, the public key into one every node mounts. The pair
  outlives restarts, so restarting the auth node doesn't sign everyone out.
- Hands the cluster-state volume to the user the nodes run as, so dbmonitor can record the
  primary there.
"""

import argparse
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

APP_UID = 10001


class SetupError(Exception):
    pass


def _pem_public(private_key) -> bytes:
    return private_key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)


def _write_new(path: str, data: bytes, mode: int) -> None:
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    except OSError as exc:
        raise SetupError(f"could not create {path}: {exc.strerror}") from None
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chown(path, APP_UID, APP_UID)
    except OSError as exc:
        raise SetupError(f"could not write {path}: {exc.strerror}") from None


def _read(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as exc:
        raise SetupError(f"could not read {path}: {exc.strerror}") from None


def ensure_keys(private_path: str, public_path: str) -> None:
    have_private = os.path.exists(private_path)
    have_public = os.path.exists(public_path)

    if have_private and have_public:
        try:
            private_key = serialization.load_pem_private_key(_read(private_path), password=None)
        except ValueError as exc:
            raise SetupError(f"{private_path} is not a PEM private key ({exc})") from None
        if _pem_public(private_key).strip() != _read(public_path).strip():
            raise SetupError(f"{private_path} and {public_path} are not a pair. Remove both volumes "
                             "(docker compose down -v) to make a new pair; everyone then signs in again.")
        print("setup: the token signing keys exist and match")
        return
    if have_private != have_public:
        missing = public_path if have_private else private_path
        raise SetupError(f"{missing} is missing but its other half exists. Remove both volumes "
                         "(docker compose down -v) to make a new pair; everyone then signs in again.")

    private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    private_pem = private_key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    _write_new(private_path, private_pem, 0o400)
    _write_new(public_path, _pem_public(private_key), 0o444)
    print(f"setup: made a new token signing key pair ({private_path}, {public_path})")


def prepare_state_dir(path: str) -> None:
    try:
        os.chown(path, APP_UID, APP_UID)
        os.chmod(path, 0o755)
        for entry in os.listdir(path):
            os.chown(os.path.join(path, entry), APP_UID, APP_UID)
    except OSError as exc:
        raise SetupError(f"could not hand {path} to the node user: {exc.strerror}") from None
    print(f"setup: {path} belongs to the node user")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--private", required=True)
    parser.add_argument("--public", required=True)
    parser.add_argument("--state-dir", required=True)
    args = parser.parse_args()
    try:
        ensure_keys(args.private, args.public)
        prepare_state_dir(args.state_dir)
    except SetupError as exc:
        print(f"setup failed: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
