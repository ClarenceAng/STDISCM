"""Database connection pooling (bonus 3).

Opening a PostgreSQL connection costs a TCP handshake, password authentication and a new server
process. A pool keeps connections open and lends them to the node's request threads instead:
a thread borrows one with acquire(), runs its transaction and hands it back with release().

Each node keeps two pools, one to whichever database node is the primary and one to the standby.
The pool is written here rather than taken from a library so that it reports every failure in its
own words and reconnects at once after a failover instead of after a growing back-off.
"""

import logging
import threading
import time
import weakref
from dataclasses import dataclass

import psycopg
from psycopg import pq
from psycopg.conninfo import make_conninfo
from psycopg.rows import dict_row

from .errors import DatabaseUnavailable, PoolExhausted
from .hosts import HostResolver
from .watchdog import QueryWatchdog

log = logging.getLogger("pool")


def one_line(exc: BaseException) -> str:
    """psycopg's messages span several lines (one per host tried); the status page wants one."""
    parts = [part.strip() for part in str(exc).splitlines() if part.strip()]
    return "; ".join(parts) if parts else type(exc).__name__


@dataclass(frozen=True)
class PoolSettings:
    hosts: list[str]
    port: int
    dbname: str
    user: str
    password: str
    max_size: int
    wait_timeout: float       # longest a request waits for a connection when all are lent out
    connect_timeout: int      # per database node tried, in whole seconds (libpq's minimum is 2)
    max_idle: float           # an idle connection older than this is closed instead of reused
    session_options: str      # server settings for every connection (lock_timeout and so on)


class ConnectionPool:
    def __init__(self, node: str, name: str, target_session_attrs: str, settings: PoolSettings,
                 resolver: HostResolver):
        self.name = name
        self._node = node
        self._target = target_session_attrs   # "read-write" finds the primary, "standby" the standby
        self._s = settings
        self._resolver = resolver
        self._names: weakref.WeakKeyDictionary = weakref.WeakKeyDictionary()   # connection -> database node
        self._cond = threading.Condition()
        self._idle: list[tuple[psycopg.Connection, float]] = []   # newest last, so reuse is LIFO
        self._open = 0          # connections that exist: idle, lent out, or being opened
        self._in_use = 0
        self._preferred = settings.hosts[0]   # the host the last connection went to is tried first
        self._connections_made = 0
        self._connect_failures = 0
        self._checkouts = 0
        self._reused = 0
        self._waited = 0
        self._wait_timeouts = 0
        self._discarded = 0
        self._last_host = ""
        self._last_error = ""

    # ---- lending ----------------------------------------------------------------------------

    def acquire(self, wait_limit: float | None = None) -> psycopg.Connection:
        """Lends out an idle connection, or opens a new one if fewer than max_size exist, or waits up
        to wait_timeout (or wait_limit, if shorter) for one to come back. Raises DatabaseUnavailable
        if a new connection can't be opened and PoolExhausted if the wait runs out."""
        wait = self._s.wait_timeout if wait_limit is None else max(0.0, min(self._s.wait_timeout, wait_limit))
        deadline = time.monotonic() + wait
        stale: list[psycopg.Connection] = []
        try:
            with self._cond:
                self._checkouts += 1
                counted_wait = False
                while True:
                    conn = self._take_idle(stale)
                    if conn is not None:
                        self._in_use += 1
                        self._reused += 1
                        return conn
                    if self._open < self._s.max_size:
                        self._open += 1   # hold the slot while connecting outside the lock
                        break
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        self._wait_timeouts += 1
                        raise PoolExhausted(
                            f"all {self._s.max_size} {self.name} database connections stayed in use "
                            f"for {wait:g} s")
                    if not counted_wait:
                        self._waited += 1
                        counted_wait = True
                    self._cond.wait(remaining)
        finally:
            for conn in stale:
                self._close_quietly(conn, "it sat idle too long or had broken")

        try:
            conn = self._connect()
        except BaseException:
            with self._cond:
                self._open -= 1
                self._cond.notify()
            raise
        with self._cond:
            self._in_use += 1
        return conn

    def release(self, conn: psycopg.Connection) -> None:
        """Takes a connection back. One that broke or is still inside a transaction is closed
        rather than lent to the next request."""
        discard = (conn.closed or conn.broken
                   or conn.info.transaction_status != pq.TransactionStatus.IDLE)
        with self._cond:
            self._in_use -= 1
            if discard:
                self._open -= 1
                self._discarded += 1
            else:
                self._idle.append((conn, time.monotonic()))
            self._cond.notify()
        if discard and not conn.closed:
            self._close_quietly(conn, "it was returned broken or mid-transaction")

    def node_of(self, conn: psycopg.Connection) -> str:
        """The database node a connection leads to, as that node named itself."""
        return self._names.get(conn, "unknown")

    def forget_host(self, host: str) -> int:
        """`host` stopped answering. Its idle connections died or froze with it, so they are closed
        before another request borrows one and waits, and new connections try the other node first."""
        with self._cond:
            keep, gone = [], []
            for conn, since in self._idle:
                (gone if self.node_of(conn) == host else keep).append((conn, since))
            self._idle = keep
            self._open -= len(gone)
            self._discarded += len(gone)
            if self._preferred == host:
                others = [h for h in self._s.hosts if h != host]
                if others:
                    self._preferred = others[0]
        for conn, _ in gone:
            self._close_quietly(conn, f"{host} stopped answering")
        return len(gone)

    def close(self) -> None:
        with self._cond:
            idle, self._idle = self._idle, []
            self._open -= len(idle)
        for conn, _ in idle:
            self._close_quietly(conn, "the node is stopping")

    def stats(self) -> dict:
        with self._cond:
            return {
                "name": self.name,
                "max_size": self._s.max_size,
                "open": self._open,
                "in_use": self._in_use,
                "idle": len(self._idle),
                "connections_made": self._connections_made,
                "connect_failures": self._connect_failures,
                "checkouts": self._checkouts,
                "reused": self._reused,
                "waited": self._waited,
                "wait_timeouts": self._wait_timeouts,
                "discarded": self._discarded,
                "last_host": self._last_host,
                "last_error": self._last_error,
            }

    # ---- internals ----------------------------------------------------------------------------

    def _take_idle(self, stale: list) -> psycopg.Connection | None:
        """Called with the lock held. Connections to close are put in `stale` and closed after
        the lock is released, so a slow close doesn't hold up other threads."""
        now = time.monotonic()
        while self._idle:
            conn, since = self._idle.pop()
            if conn.closed or conn.broken or now - since > self._s.max_idle:
                self._open -= 1
                self._discarded += 1
                stale.append(conn)
                continue
            return conn
        return None

    def _connect(self) -> psycopg.Connection:
        order = [self._preferred] + [h for h in self._s.hosts if h != self._preferred]
        usable = self._resolver.usable(order)
        if not usable:
            raise self._connect_failed(f"could not connect to the {self.name} database: no database node "
                                       f"has an address right now ({self._resolver.problems()})")
        conninfo = make_conninfo(
            host=",".join(h for h, _ in usable),
            hostaddr=",".join(a for _, a in usable),   # no DNS lookup while connecting
            port=",".join([str(self._s.port)] * len(usable)),
            dbname=self._s.dbname,
            user=self._s.user,
            password=self._s.password,
            connect_timeout=self._s.connect_timeout,
            target_session_attrs=self._target,
            application_name=f"{self._node}-{self.name}",
            keepalives=1, keepalives_idle=10, keepalives_interval=3, keepalives_count=3,
            options=self._s.session_options,
        )
        try:
            conn = psycopg.connect(conninfo, autocommit=False, row_factory=dict_row)
        except psycopg.OperationalError as exc:
            raise self._connect_failed(f"could not connect to the {self.name} database ({one_line(exc)})") from exc
        try:
            # Ask the server who it is: Docker can give a stopped node's address to the other node
            # before the lookup that would notice has finished, so the address alone can mislead.
            with QueryWatchdog(conn, self._s.connect_timeout):
                host = conn.execute("SELECT current_setting('cluster_name') AS name").fetchone()["name"]
                conn.rollback()
        except psycopg.Error as exc:
            self._close_quietly(conn, "it failed right after connecting")
            raise self._connect_failed(f"the {self.name} database stopped answering right after "
                                       f"connecting ({one_line(exc)})") from exc
        host = host or conn.info.host
        self._names[conn] = host
        with self._cond:
            self._connections_made += 1
            self._last_host = host
            self._preferred = host
            self._last_error = ""   # the status page shows current trouble; the log keeps the history
            open_now = self._open
        log.info("%s pool: opened a connection to %s (%d of %d open)", self.name, host, open_now, self._s.max_size)
        return conn

    def _connect_failed(self, reason: str) -> DatabaseUnavailable:
        with self._cond:
            self._connect_failures += 1
            self._last_error = reason
        return DatabaseUnavailable(reason)

    def _close_quietly(self, conn: psycopg.Connection, why: str) -> None:
        # Closing only sends a goodbye message and frees the socket. If even that fails the
        # connection is already gone, which is what we wanted, so the error is logged and dropped.
        try:
            conn.close()
        except (psycopg.Error, OSError) as exc:
            log.info("%s pool: closing a connection because %s failed too: %s", self.name, why, one_line(exc))
