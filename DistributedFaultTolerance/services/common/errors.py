"""The ways a request can fail. common.rpc turns each one into its own gRPC status, so the view node
can tell the user which of them happened."""

import grpc


class AppError(Exception):
    """A request the node refuses on purpose, with the status and the reason shown to the user."""

    def __init__(self, code: grpc.StatusCode, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def invalid(message: str) -> AppError:
    return AppError(grpc.StatusCode.INVALID_ARGUMENT, message)


def not_found(message: str) -> AppError:
    return AppError(grpc.StatusCode.NOT_FOUND, message)


def refused(message: str) -> AppError:
    """The request is well formed but the current state doesn't allow it (a full or closed section)."""
    return AppError(grpc.StatusCode.FAILED_PRECONDITION, message)


def conflict(message: str) -> AppError:
    return AppError(grpc.StatusCode.ALREADY_EXISTS, message)


def forbidden(message: str) -> AppError:
    return AppError(grpc.StatusCode.PERMISSION_DENIED, message)


def unauthenticated(message: str) -> AppError:
    return AppError(grpc.StatusCode.UNAUTHENTICATED, message)


def busy(message: str) -> AppError:
    """Another request holds what this one needs; trying again shortly will work."""
    return AppError(grpc.StatusCode.ABORTED, message)


class DatabaseUnavailable(Exception):
    """No database node could serve the request: it can't be reached, it stopped answering, or the
    only reachable node can't do what was asked (a write while only the standby is up)."""


class PoolExhausted(Exception):
    """The database is fine, but every connection in this node's pool stayed lent out for the whole wait."""


class CallerGone(Exception):
    """The caller's deadline passed or it cancelled, so the work was abandoned and nothing was saved.
    Nobody receives the answer; it is only logged."""


class CommitOutcomeUnknown(Exception):
    """The connection broke while COMMIT was on its way, so the change may or may not have been saved."""
