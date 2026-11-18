"""Runs a node's database work, one transaction at a time, through its two connection pools.

Writes go to the primary. Reads go to the primary too, and fall back to the standby when the
primary can't be reached, so pages that only read keep working while the database fails over.
"""

import contextvars
import logging
import time
from typing import Callable, TypeVar

import psycopg
from psycopg import errors as pg_errors

from . import config
from .errors import CallerGone, CommitOutcomeUnknown, DatabaseUnavailable, busy
from .hosts import HostResolver
from .pool import ConnectionPool, PoolSettings, one_line
from .watchdog import QueryWatchdog

log = logging.getLogger("db")

T = TypeVar("T")


class _Request:
    """What the database layer knows about the gRPC request it is working for."""

    def __init__(self, context):
        self.context = context      # the gRPC call: its deadline and whether the caller still waits
        self.sources: set[str] = set()   # database nodes that served it, for the "from the standby" note

    def time_left(self) -> float | None:
        if self.context is None:
            return None
        left = self.context.time_remaining()
        return None if left is None else max(0.0, left)

    def caller_waiting(self) -> bool:
        return self.context is None or self.context.is_active()


# A request runs on one gRPC worker thread, which a context variable follows.
_request: contextvars.ContextVar[_Request | None] = contextvars.ContextVar("db_request", default=None)


def begin_request(context=None) -> contextvars.Token:
    return _request.set(_Request(context))


def end_request(token: contextvars.Token) -> None:
    _request.reset(token)


def served_by_standby() -> bool:
    req = _request.get()
    return req is not None and "standby" in req.sources


def _note_source(name: str) -> None:
    req = _request.get()
    if req is not None:
        req.sources.add(name)


def _time_left() -> float | None:
    req = _request.get()
    return None if req is None else req.time_left()


class Database:
    PRIMARY_RETRY_AFTER = 3.0   # seconds reads go to the standby first after the primary failed
    CHECK_TIMEOUT = 1.0         # seconds a pooled connection gets to answer SELECT 1

    def __init__(self, node: str):
        self._primary_down_until = 0.0
        hosts = config.host_list("DB_HOSTS", "db1,db2")
        port = config.integer("DB_PORT", 5432, 1, 65535)
        dbname = config.text("DB_NAME", "enrollment")
        user = config.text("DB_USER", "app")
        password = config.text("DB_PASSWORD")
        self.lock_timeout = config.seconds("DB_LOCK_TIMEOUT", 2.0, 0.1, 60)
        statement_timeout = config.seconds("DB_STATEMENT_TIMEOUT", 3.0, 0.1, 120)
        self.query_timeout = config.seconds("DB_QUERY_TIMEOUT", 5.0, 0.5, 120)
        idle_in_transaction = config.seconds("DB_IDLE_IN_TRANSACTION_TIMEOUT", 5.0, 1, 600)
        # lock_timeout: a request waiting for a row lock gives up instead of queueing for ever.
        # idle_in_transaction_session_timeout: a node that freezes while holding a row lock loses
        # it after this long, so it can't block a section for everyone else.
        options = (f"-c lock_timeout={int(self.lock_timeout * 1000)}"
                   f" -c statement_timeout={int(statement_timeout * 1000)}"
                   f" -c idle_in_transaction_session_timeout={int(idle_in_transaction * 1000)}")

        def settings(max_size: int) -> PoolSettings:
            return PoolSettings(
                hosts=hosts, port=port, dbname=dbname, user=user, password=password,
                max_size=max_size,
                wait_timeout=config.seconds("DB_POOL_WAIT", 2.0, 0.1, 60),
                connect_timeout=config.integer("DB_CONNECT_TIMEOUT", 2, 2, 60),
                max_idle=config.seconds("DB_POOL_MAX_IDLE", 300.0, 1, 86400),
                session_options=options,
            )

        self.resolver = HostResolver(hosts, port)
        self.primary = ConnectionPool(node, "primary", "read-write",
                                      settings(config.integer("DB_POOL_SIZE", 10, 1, 200)), self.resolver)
        self.standby = ConnectionPool(node, "standby", "standby",
                                      settings(config.integer("DB_STANDBY_POOL_SIZE", 4, 1, 200)), self.resolver)

    def close(self) -> None:
        self.primary.close()
        self.standby.close()

    def write(self, work: Callable[[psycopg.Connection], T]) -> T:
        """Runs `work` in one transaction on the primary and commits it."""
        try:
            result = self._run(self.primary, work, writes=True)
        except DatabaseUnavailable:
            self._primary_failed()
            raise
        self._primary_down_until = 0.0
        _note_source("primary")
        return result

    def read(self, work: Callable[[psycopg.Connection], T]) -> T:
        """Runs `work` on the primary, or on the standby if the primary can't be reached.
        For a few seconds after the primary failed, reads go to the standby first, so a request
        doesn't spend its time on a primary that just stopped answering."""
        order = [(self.primary, "primary"), (self.standby, "standby")]
        if time.monotonic() < self._primary_down_until:
            order.reverse()
        problems = []
        for pool, name in order:
            try:
                result = self._run(pool, work, writes=False)
            except DatabaseUnavailable as exc:
                problems.append(f"{name.capitalize()}: {exc}")
                if name == "primary":
                    self._primary_failed()
                if len(problems) == 1:
                    log.warning("the %s can't serve a read (%s); trying the other database node", name, exc)
                continue
            if name == "primary":
                self._primary_down_until = 0.0
            _note_source(name)
            return result
        raise DatabaseUnavailable("neither database node can answer. " + ". ".join(problems))

    def _primary_failed(self) -> None:
        self._primary_down_until = time.monotonic() + self.PRIMARY_RETRY_AFTER

    # ---- internals ----------------------------------------------------------------------------

    def _run(self, pool: ConnectionPool, work: Callable[[psycopg.Connection], T], *, writes: bool) -> T:
        attempt = 0
        while True:
            attempt += 1
            # Deadline propagation: the work gets only the time the caller has left, so a node
            # doesn't keep working on a request nobody is waiting for any more.
            left = _time_left()
            if left is not None and left <= 0.05:
                raise CallerGone("the caller's deadline passed before the database work could start")
            conn = pool.acquire(wait_limit=left)
            host = pool.node_of(conn)
            limit = self.query_timeout if left is None else max(0.1, min(self.query_timeout, _time_left() or 0.1))
            # A pooled connection can have died or frozen with its server (a failover) since it was
            # last used. Finding out first, before any work, makes a retry safe. SELECT 1 answers in
            # milliseconds, so it gets a short watchdog of its own.
            limit = min(limit, self.CHECK_TIMEOUT)
            watchdog = QueryWatchdog(conn, limit)
            phase = "check"
            try:
                with watchdog:
                    conn.execute("SELECT 1")
                phase = "work"
                limit = self.query_timeout if left is None else max(0.1, min(self.query_timeout, _time_left() or 0.1))
                watchdog = QueryWatchdog(conn, limit)
                with watchdog:
                    result = work(conn)
                    req = _request.get()
                    if writes and req is not None and not req.caller_waiting():
                        # The caller was already told the request failed (deadline or cancel), so
                        # saving the change now would surprise them. Undo it instead.
                        raise CallerGone("the caller stopped waiting before the change was committed, "
                                         "so it was rolled back")
                    phase = "commit"
                    conn.commit()
            except psycopg.Error as exc:
                lost = conn.closed or conn.broken
                if not lost:
                    self._roll_back(conn, pool)
                    if isinstance(exc, pg_errors.ReadOnlySqlTransaction):
                        # only possible if this database node became a standby after we connected;
                        # closed before release so no other request borrows it
                        self._close_after(conn, "it now leads to a read-only standby")
                pool.release(conn)

                if lost:
                    if watchdog.fired:
                        reason = f"it did not answer within {limit:.1f} s, so this node gave up on it"
                    else:
                        reason = f"the connection was lost ({one_line(exc)})"
                    closed = pool.forget_host(host)
                    if closed:
                        log.info("closed %d idle %s connection(s) to %s, which stopped answering", closed, pool.name, host)
                    if phase == "check" and attempt == 1:
                        # nothing has run yet, so trying again on a new connection is safe
                        log.info("a pooled %s connection to %s failed its check (%s); retrying on a new one",
                                 pool.name, host, reason)
                        continue
                    if phase == "commit":
                        if writes:
                            raise CommitOutcomeUnknown(
                                f"The {pool.name} database connection was lost while committing ({reason}), "
                                "so the change may or may not have been saved. Reload the page to check.") from exc
                        # a read has its answer already; there was nothing to commit
                        log.warning("the %s connection broke after a read had finished (%s); the read stands", pool.name, reason)
                        return result
                    raise DatabaseUnavailable(f"the {pool.name} database: {reason}") from exc

                # the server refused a statement, but the connection itself is fine
                if isinstance(exc, pg_errors.ReadOnlySqlTransaction):
                    if attempt == 1:
                        log.warning("the %s connection leads to a read-only node now; retrying on a new one", pool.name)
                        continue
                    raise DatabaseUnavailable(f"the {pool.name} database is read-only right now (a failover is under way)") from exc
                if isinstance(exc, pg_errors.TransactionRollbackError):   # deadlock or serialization failure
                    if attempt == 1:
                        log.info("the database rolled a transaction back (%s); retrying it once", one_line(exc))
                        continue
                    raise busy("The database rolled this request back twice because it collided with another one. "
                               "Try again.") from exc
                if isinstance(exc, pg_errors.LockNotAvailable):
                    raise busy(f"Another request held a record this one needs for over {self.lock_timeout:g} s, "
                               "so this one gave up. Try again.") from exc
                if isinstance(exc, pg_errors.QueryCanceled):
                    raise DatabaseUnavailable(
                        f"the {pool.name} database cancelled a statement that ran past its time limit") from exc
                raise
            except BaseException:
                # the work refused the request (AppError) or failed in Python: undo and pass it on
                if not (conn.closed or conn.broken):
                    self._roll_back(conn, pool)
                pool.release(conn)
                raise
            else:
                pool.release(conn)
                return result

    @staticmethod
    def _roll_back(conn: psycopg.Connection, pool: ConnectionPool) -> None:
        try:
            conn.rollback()
        except psycopg.Error as exc:
            # the pool closes it on release, because it is not idle any more
            log.warning("rolling back on a %s connection failed (%s); it will be discarded", pool.name, one_line(exc))

    @staticmethod
    def _close_after(conn: psycopg.Connection, why: str) -> None:
        try:
            conn.close()
        except psycopg.Error as exc:
            log.info("closing a connection because %s failed too: %s", why, one_line(exc))
