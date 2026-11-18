"""The wrapper every controller method goes through."""

import functools
import logging
import time

import grpc
import psycopg

from . import db
from .errors import AppError, CallerGone, CommitOutcomeUnknown, DatabaseUnavailable, PoolExhausted
from .pool import one_line

log = logging.getLogger("rpc")


def _send_trailers(context: grpc.ServicerContext, *, database_error: bool) -> None:
    """Trailing metadata tells the view node what it can't see in the status code alone:
    error-origin=database: the node is fine but its database isn't (UNAVAILABLE means two things);
    db-source=standby: the answer was read from the standby, so the newest changes may be missing."""
    trailers = []
    if database_error:
        trailers.append(("error-origin", "database"))
    if db.served_by_standby():
        trailers.append(("db-source", "standby"))
    if trailers:
        context.set_trailing_metadata(tuple(trailers))


def _abort(context, code: grpc.StatusCode, message: str, *, database_error: bool = False):
    _send_trailers(context, database_error=database_error)
    context.abort(code, message)   # raises, ending the call


def handler(*roles: str, public: bool = False):
    """Wraps a gRPC method of a controller.

    Unless `public`, it first checks the caller's session token and role and passes the caller to
    the method as `user`. Then it runs the method and turns each kind of failure into its own
    status with a message for the user, and logs every call with its outcome.
    """

    def decorate(method):
        name = method.__name__

        @functools.wraps(method)
        def wrapper(self, request, context):
            node = self.ctx.node
            started = time.monotonic()
            request_token = db.begin_request(context)
            who = "anonymous"

            def took() -> str:
                return f"{(time.monotonic() - started) * 1000:.0f} ms"

            try:
                if public:
                    reply = method(self, request, context)
                else:
                    user = self.ctx.verifier.authenticate(context, roles)
                    who = f"{user.username} ({user.role})"
                    reply = method(self, request, context, user)
                _send_trailers(context, database_error=False)
                log.info("%s by %s: OK in %s%s", name, who, took(),
                         " (read from the standby)" if db.served_by_standby() else "")
                return reply
            except AppError as exc:
                log.info("%s by %s: %s in %s: %s", name, who, exc.code.name, took(), exc.message)
                _abort(context, exc.code, exc.message)
            except PoolExhausted as exc:
                log.warning("%s by %s: pool exhausted in %s: %s", name, who, took(), exc)
                _abort(context, grpc.StatusCode.RESOURCE_EXHAUSTED,
                       f"The {node} node is too busy right now: {exc}. Try again.", database_error=True)
            except DatabaseUnavailable as exc:
                log.warning("%s by %s: database unavailable in %s: %s", name, who, took(), exc)
                _abort(context, grpc.StatusCode.UNAVAILABLE, str(exc), database_error=True)
            except CallerGone as exc:
                log.warning("%s by %s: abandoned after %s: %s", name, who, took(), exc)
                _abort(context, grpc.StatusCode.DEADLINE_EXCEEDED, f"Nothing was saved: {exc}.", database_error=True)
            except CommitOutcomeUnknown as exc:
                log.error("%s by %s: commit outcome unknown in %s: %s", name, who, took(), exc)
                _abort(context, grpc.StatusCode.UNKNOWN, str(exc), database_error=True)
            except psycopg.Error as exc:
                log.exception("%s by %s: database error in %s", name, who, took())
                _abort(context, grpc.StatusCode.INTERNAL,
                       f"The database refused a statement on the {node} node "
                       f"(SQLSTATE {exc.sqlstate or 'none'}): {one_line(exc)}", database_error=True)
            except Exception as exc:
                log.exception("%s by %s: unexpected error in %s", name, who, took())
                _abort(context, grpc.StatusCode.INTERNAL,
                       f"Unexpected {type(exc).__name__} on the {node} node: {exc}")
            finally:
                db.end_request(request_token)

        return wrapper

    return decorate
