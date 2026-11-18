"""Checks the session token that comes with every request.

The auth node signs each token with its private key (RS256). Every node checks the signature here
with the public key, so it never has to ask the auth node, and keeps working while that node is
down. The token names a row in the sessions table, which every node also checks, so a logout on
the auth node ends the session on all nodes at once.
"""

import hashlib
import logging
import uuid
from dataclasses import dataclass

import grpc
import jwt
from cryptography.hazmat.primitives import serialization

from .config import ConfigError
from .db import served_by_standby
from .errors import DatabaseUnavailable, forbidden, unauthenticated

log = logging.getLogger("sessions")

ISSUER = "auth-node"
AUDIENCE = "enrollment-system"
ROLES = ("student", "faculty", "admin")


@dataclass(frozen=True)
class CurrentUser:
    id: int
    username: str
    full_name: str
    role: str
    session_id: str


def key_id(public_key) -> str:
    """A short fingerprint of the public key. The auth node puts it in each token's header, so a
    token signed with some other key is reported as that rather than as a bad signature."""
    der = public_key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return hashlib.sha256(der).hexdigest()[:16]


def load_public_key(pem: bytes):
    try:
        return serialization.load_pem_public_key(pem)
    except ValueError as exc:
        raise ConfigError(f"the public key file is not a PEM public key ({exc})") from None


def _article(role: str) -> str:
    return f"an {role}" if role[0] in "aeiou" else f"a {role}"


class SessionVerifier:
    def __init__(self, public_pem: bytes, db):
        self._key = load_public_key(public_pem)
        self.kid = key_id(self._key)
        self._db = db

    @staticmethod
    def token_from(context: grpc.ServicerContext) -> str:
        for key, value in context.invocation_metadata():
            if key == "authorization":
                if not value.startswith("Bearer "):
                    raise unauthenticated("The request's authorization header is not a Bearer token.")
                return value[len("Bearer "):]
        raise unauthenticated("You are not signed in.")

    def decode(self, token: str, *, verify_exp: bool = True) -> dict:
        """Checks the signature and claims. Raises AppError(UNAUTHENTICATED) saying what is wrong."""
        try:
            header = jwt.get_unverified_header(token)
        except jwt.DecodeError:
            raise unauthenticated("The session token is malformed. Sign in again.") from None
        if header.get("kid") != self.kid:
            raise unauthenticated("The session token was signed with a key this node doesn't trust "
                                  "(were the keys made again?). Sign in again.")
        try:
            claims = jwt.decode(
                token, self._key, algorithms=["RS256"], audience=AUDIENCE, issuer=ISSUER,
                options={"require": ["exp", "iat", "sub", "sid", "role"], "verify_exp": verify_exp},
            )
        except jwt.ExpiredSignatureError:
            raise unauthenticated("Your session has expired. Sign in again.") from None
        except jwt.InvalidSignatureError:
            raise unauthenticated("The session token's signature is wrong, so it was not issued by the auth node. "
                                  "Sign in again.") from None
        except jwt.MissingRequiredClaimError as exc:
            raise unauthenticated(f"The session token has no '{exc.claim}' claim. Sign in again.") from None
        except jwt.InvalidAudienceError:
            raise unauthenticated("The session token was issued for another system. Sign in again.") from None
        except jwt.InvalidIssuerError:
            raise unauthenticated("The session token was not issued by the auth node. Sign in again.") from None
        except jwt.ImmatureSignatureError:
            raise unauthenticated("The session token is not valid yet (are the nodes' clocks apart?). "
                                  "Sign in again.") from None
        except jwt.InvalidTokenError as exc:
            raise unauthenticated(f"The session token is not valid ({exc}). Sign in again.") from None
        try:
            uuid.UUID(str(claims["sid"]))
            int(claims["sub"])
        except ValueError:
            raise unauthenticated("The session token's session or user id is malformed. Sign in again.") from None
        if claims["role"] not in ROLES:
            raise unauthenticated(f"The session token has an unknown role '{claims['role']}'. Sign in again.")
        return claims

    def authenticate(self, context: grpc.ServicerContext, roles: tuple[str, ...]) -> CurrentUser:
        """Who is calling: a valid token for a session that is still open, with one of `roles`
        (any role when empty)."""
        claims = self.decode(self.token_from(context))

        def look_up(conn):
            return conn.execute(
                """SELECT s.user_id, s.revoked_at, s.expires_at < now() AS expired,
                          u.username, u.full_name, u.role
                     FROM sessions s JOIN users u ON u.id = s.user_id
                    WHERE s.id = %s""",
                (claims["sid"],),
            ).fetchone()

        row = self._db.read(look_up)
        if row is None and served_by_standby():
            # The standby can be behind the primary (it was down while this session was made), so
            # a missing row proves nothing. Signing the user out for it would be wrong.
            raise DatabaseUnavailable("this session can't be checked right now: only the standby database "
                                      "answers, and it doesn't have this session yet")
        if row is None:
            raise unauthenticated("This session is unknown to the database (was the database reset?). Sign in again.")
        if row["revoked_at"] is not None:
            raise unauthenticated("You signed out of this session. Sign in again.")
        if row["expired"]:
            raise unauthenticated("Your session has expired. Sign in again.")
        if row["user_id"] != int(claims["sub"]):
            raise unauthenticated("The session token doesn't match its session. Sign in again.")

        user = CurrentUser(id=row["user_id"], username=row["username"], full_name=row["full_name"],
                           role=row["role"], session_id=str(claims["sid"]))
        if roles and user.role not in roles:
            raise forbidden(f"This needs {' or '.join(_article(r) for r in roles)} account, "
                            f"and you are signed in as {_article(user.role)}.")
        return user
