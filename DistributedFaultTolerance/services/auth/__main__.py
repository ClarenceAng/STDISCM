from cryptography.hazmat.primitives import serialization

import auth_pb2_grpc
from common import config
from common.config import ConfigError
from common.server import run_node
from common.sessions import key_id

from .controller import AuthController


def register(server, ctx):
    path = config.text("PRIVATE_KEY_PATH")
    try:
        private_key = serialization.load_pem_private_key(config.read_file("PRIVATE_KEY_PATH", path), password=None)
    except ValueError as exc:
        raise ConfigError(f"{path} is not a PEM private key ({exc})") from None
    if key_id(private_key.public_key()) != ctx.verifier.kid:
        raise ConfigError(f"{path} does not belong to the public key the other nodes check tokens with")
    minutes = config.integer("SESSION_MINUTES", 120, 1, 7 * 24 * 60)
    auth_pb2_grpc.add_AuthServiceServicer_to_server(AuthController(ctx, private_key, minutes * 60), server)


run_node("auth", register)
