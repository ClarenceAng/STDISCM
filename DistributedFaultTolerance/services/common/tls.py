"""TLS for the gRPC links between nodes.

run.bat / run.sh make the certificates with mkcert before starting the containers. Each node has
its own certificate, for its own name (the name the view dials, such as "auth"), signed by
mkcert's certificate authority. docker-compose.yml mounts certs/<node> at /tls in that node only,
and the authority's certificate (never its key) at /ca/ca.pem in the nodes that make calls.

gRPC only reports that a secure port or channel failed, not why, so the files are checked here
first and each problem gets its own message.
"""

import grpc
from cryptography import x509
from cryptography.exceptions import UnsupportedAlgorithm
from cryptography.hazmat.primitives import serialization

from . import config
from .config import ConfigError

MADE_BY = "run.bat or run.sh, with mkcert"


def _certificate(name: str, path: str, data: bytes) -> x509.Certificate:
    try:
        certificates = x509.load_pem_x509_certificates(data)
    except ValueError as exc:
        raise ConfigError(f"{name}: {path} is not a PEM certificate ({exc})") from None
    return certificates[0]   # the node's own; any after it are the chain that signed it


def _public_der(key) -> bytes:
    return key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)


def server_credentials() -> grpc.ServerCredentials:
    """This node's certificate and private key, checked to be a pair. Raises ConfigError."""
    cert_path = config.text("TLS_CERT_FILE", "/tls/cert.pem")
    key_path = config.text("TLS_KEY_FILE", "/tls/key.pem")
    cert_pem = config.read_file("TLS_CERT_FILE", cert_path, MADE_BY)
    key_pem = config.read_file("TLS_KEY_FILE", key_path, MADE_BY)

    certificate = _certificate("TLS_CERT_FILE", cert_path, cert_pem)
    try:
        key = serialization.load_pem_private_key(key_pem, password=None)
    except TypeError:
        raise ConfigError(f"TLS_KEY_FILE: {key_path} is protected by a password, which a node can't type in") from None
    except ValueError as exc:
        raise ConfigError(f"TLS_KEY_FILE: {key_path} is not a PEM private key ({exc})") from None
    except UnsupportedAlgorithm as exc:
        raise ConfigError(f"TLS_KEY_FILE: {key_path} is a kind of key this node can't use ({exc})") from None
    if _public_der(certificate.public_key()) != _public_der(key.public_key()):
        raise ConfigError(f"{key_path} is not the key of {cert_path}; they were made separately. "
                          "Run run.bat (or run.sh) again to make a new pair.")

    return grpc.ssl_server_credentials([(key_pem, cert_pem)])


def channel_credentials() -> grpc.ChannelCredentials:
    """Trusts mkcert's certificate authority and nothing else. Raises ConfigError."""
    path = config.text("GRPC_CA_FILE", "/ca/ca.pem")
    pem = config.read_file("GRPC_CA_FILE", path, MADE_BY)
    _certificate("GRPC_CA_FILE", path, pem)
    return grpc.ssl_channel_credentials(root_certificates=pem)
