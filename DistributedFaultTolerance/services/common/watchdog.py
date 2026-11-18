"""The watchdog that turns a frozen database server into an error instead of a hang."""

import logging
import os
import socket
import threading

import psycopg

log = logging.getLogger("db")


def _one_line(exc: BaseException) -> str:
    parts = [part.strip() for part in str(exc).splitlines() if part.strip()]
    return "; ".join(parts) if parts else type(exc).__name__


class QueryWatchdog:
    """Closes a connection's socket if the database hasn't answered within `seconds`.

    A frozen database server (docker pause, a hung VM) still has its kernel acknowledge TCP
    packets, so neither TCP keepalives nor libpq notice anything wrong, and the request thread
    would wait for ever while holding a pool connection. Shutting the socket down from this
    timer thread wakes the waiting thread with a connection error instead.
    """

    def __init__(self, conn: psycopg.Connection, seconds: float):
        self._conn = conn
        self._seconds = seconds
        self._lock = threading.Lock()
        self._done = False
        self.fired = False
        self._timer = threading.Timer(seconds, self._fire)
        self._timer.daemon = True

    def __enter__(self) -> "QueryWatchdog":
        self._timer.start()
        return self

    def __exit__(self, *exc_info) -> bool:
        # after this the timer can no longer touch the socket, so the connection is safe to reuse
        with self._lock:
            self._done = True
        self._timer.cancel()
        return False

    def _fire(self) -> None:
        with self._lock:
            if self._done:
                return
            self.fired = True
            try:
                fd = self._conn.pgconn.socket
            except psycopg.OperationalError as exc:
                log.warning("watchdog: after %g s the connection was already closed (%s)", self._seconds, _one_line(exc))
                return
            try:
                sock = socket.socket(fileno=os.dup(fd))
            except OSError as exc:
                log.error("watchdog: could not take hold of the stuck connection's socket: %s", exc.strerror)
                return
            try:
                sock.shutdown(socket.SHUT_RDWR)
                log.warning("watchdog: the database did not answer within %g s, so the connection was shut down", self._seconds)
            except OSError as exc:
                log.error("watchdog: could not shut the stuck connection's socket down: %s", exc.strerror)
            finally:
                sock.close()   # only the duplicate descriptor; psycopg still owns the original
