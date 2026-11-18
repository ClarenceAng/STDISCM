"""The auth node's controller."""

import logging

import bcrypt
import grpc
import jwt

import auth_pb2
import auth_pb2_grpc
import common_pb2
from common.errors import AppError, invalid, unauthenticated
from common.rpc import handler
from common.sessions import AUDIENCE, ISSUER

from . import model

log = logging.getLogger("auth")

# Deliberately the same for an unknown username and a wrong password, so the login form can't be
# used to find out which usernames exist. The log records which of the two it was.
WRONG_LOGIN = "Wrong username or password."


class AuthController(auth_pb2_grpc.AuthServiceServicer):
    def __init__(self, ctx, private_key, session_seconds: int):
        self.ctx = ctx
        self._private_key = private_key
        self._session_seconds = session_seconds

    @handler(public=True)
    def Login(self, request, context):
        username = request.username.strip()
        if not username or not request.password:
            raise invalid("Enter both a username and a password.")
        if len(username) > 64 or len(request.password) > 200:
            raise invalid("The username or password is too long.")

        user = self.ctx.db.read(lambda conn: model.find_user(conn, username))
        if user is None:
            log.info("login refused for %r: there is no such user", username)
            raise unauthenticated(WRONG_LOGIN)
        try:
            matches = bcrypt.checkpw(request.password.encode(), user["password_hash"].encode())
        except ValueError as exc:
            log.error("the stored password hash of %r is malformed: %s", username, exc)
            raise AppError(grpc.StatusCode.INTERNAL,
                           "This account's stored password is damaged. Ask the registrar to reset it.") from None
        if not matches:
            log.info("login refused for %r: wrong password", username)
            raise unauthenticated(WRONG_LOGIN)

        # Recording the session needs the primary. While only the standby is up, the user can't
        # sign in, but everyone already signed in carries on.
        session = self.ctx.db.write(lambda conn: model.create_session(conn, user["id"], self._session_seconds))
        expires_at = int(session["expires_at"].timestamp())
        token = jwt.encode(
            {
                "iss": ISSUER,
                "aud": AUDIENCE,
                "sub": str(user["id"]),
                "sid": str(session["id"]),
                "username": user["username"],
                "name": user["full_name"],
                "role": user["role"],
                "iat": int(session["issued_at"].timestamp()),
                "exp": expires_at,
            },
            self._private_key,
            algorithm="RS256",
            headers={"kid": self.ctx.verifier.kid},
        )
        log.info("%s signed in, session %s", username, session["id"])
        return auth_pb2.LoginReply(
            token=token,
            expires_at=expires_at,
            session_id=str(session["id"]),
            user=common_pb2.User(id=user["id"], username=user["username"],
                                 full_name=user["full_name"], role=user["role"]),
        )

    @handler(public=True)
    def Logout(self, request, context):
        # An expired token can still be signed out; there is just nothing left to end.
        claims = self.ctx.verifier.decode(self.ctx.verifier.token_from(context), verify_exp=False)
        was_open = self.ctx.db.write(lambda conn: model.revoke_session(conn, claims["sid"]))
        log.info("%s signed out of session %s%s", claims.get("username", claims["sub"]), claims["sid"],
                 "" if was_open else " (it was already signed out)")
        return auth_pb2.LogoutReply(already_signed_out=not was_open)
