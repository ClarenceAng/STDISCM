"""Watches the two PostgreSQL nodes once a second and fails over when the primary stops answering.

What it does each check, with P the primary named in the state file and S the other node:

1. Synchronous replication. While S is streaming from P, P's synchronous_standby_names is "*",
   so P acknowledges a commit only after S has applied it. When S stops streaming, the setting is
   cleared, so P keeps accepting writes alone instead of waiting for a standby that isn't there.
   Whether S was in sync is recorded *before* the setting changes.
2. Failover. When P has failed `failures_needed` checks in a row, S is reachable, and S was in sync
   at the last check that reached P, S is promoted (pg_promote), its synchronous setting is
   cleared, and its name is written to the state file. If S was not in sync, it is not promoted:
   it could be missing commits users were told succeeded. The status page says so.
3. The state file is what the database nodes read when they start, and every second while they
   run. A former primary that comes back sees it has been replaced, stops (fencing), and restarts
   as the new primary's standby.

This monitor is a single arbiter. If it is down nothing fails over, but everything else keeps
working. A real deployment would use several monitors that agree by consensus (Patroni with etcd).
"""

import logging
import os
import threading
import time
from collections import deque

import psycopg
from psycopg import sql

from common.config import ConfigError
from common.hosts import HostResolver
from common.pool import one_line
from common.watchdog import QueryWatchdog

log = logging.getLogger("monitor")


class _Unreachable(Exception):
    pass


class ClusterMonitor:
    def __init__(self, nodes: list[str], state_path: str, password: str, *,
                 interval: float, failures_needed: int, probe_timeout: float):
        if len(nodes) != 2:
            raise ConfigError(f"the monitor watches exactly two database nodes, not {len(nodes)}")
        self._nodes = nodes
        self._resolver = HostResolver(nodes, 5432)
        self._state_path = state_path
        self._password = password
        self._interval = interval
        self._failures_needed = failures_needed
        self._probe_timeout = probe_timeout

        self._lock = threading.Lock()
        self._events: deque[tuple[float, str]] = deque(maxlen=50)
        self._reported: set[str] = set()      # one-off messages already logged for the current incident
        self._failures = 0
        self._standby_in_sync = False
        self._state_unsaved = False
        self._status: dict | None = None
        self._designated = self._load_state()

    # ---- the state file -----------------------------------------------------------------------

    def _load_state(self) -> str:
        try:
            with open(self._state_path, encoding="utf-8") as f:
                name = f.read().strip()
        except FileNotFoundError:
            name = ""
        except OSError as exc:
            raise ConfigError(f"cannot read {self._state_path}: {exc.strerror}") from None
        if name in self._nodes:
            self._event(f"the state file names {name} as the primary")
            return name
        if name:
            self._event(f"the state file names '{name}', which is not a database node; using {self._nodes[0]}")
        name = self._nodes[0]
        self._save_state(name)
        return name

    def _save_state(self, name: str) -> None:
        """Atomic: a node reading the file sees the old name or the new one, never half of one."""
        tmp = self._state_path + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(name + "\n")
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self._state_path)
        except OSError as exc:
            # Fencing depends on this file, so keep trying every check until it is written.
            self._state_unsaved = True
            self._event_once("state-unsaved", f"could not record {name} as the primary in {self._state_path} "
                                              f"({exc.strerror}); retrying every check")
            return
        if self._state_unsaved:
            self._event(f"recorded {name} as the primary in the state file")
        self._state_unsaved = False
        self._reported.discard("state-unsaved")

    # ---- events -------------------------------------------------------------------------------

    def _event(self, message: str) -> None:
        log.info("%s", message)
        with self._lock:
            self._events.appendleft((time.time(), message))

    def _event_once(self, key: str, message: str) -> None:
        if key not in self._reported:
            self._reported.add(key)
            self._event(message)

    def _resolved(self, *keys: str) -> None:
        for key in keys:
            self._reported.discard(key)

    # ---- probing ------------------------------------------------------------------------------

    def _connect(self, node: str) -> psycopg.Connection:
        address, problem = self._resolver.address(node)
        if address is None:
            raise _Unreachable(problem)   # its container is gone; no need to wait for a lookup
        return psycopg.connect(host=node, hostaddr=address, port=5432, dbname="postgres", user="postgres",
                               password=self._password, connect_timeout=max(2, int(self._probe_timeout)),
                               autocommit=True, application_name="dbmonitor")

    def probe(self, node: str) -> dict:
        try:
            with self._connect(node) as conn, QueryWatchdog(conn, self._probe_timeout):
                in_recovery, name = conn.execute("SELECT pg_is_in_recovery(), current_setting('cluster_name')").fetchone()
                if name != node:
                    # Docker gave this node's old address to the other node before the new lookup
                    # finished. Acting on the answer would mistake one node for the other.
                    return {"reachable": False, "error": f"its old address now leads to {name}"}
                if in_recovery:
                    row = conn.execute("SELECT status, sender_host FROM pg_stat_wal_receiver").fetchone()
                    receiving = f"{row[0]} from {row[1]}" if row else "not receiving WAL from a primary"
                    return {"reachable": True, "role": "standby", "replication": receiving}
                names = conn.execute("SHOW synchronous_standby_names").fetchone()[0]
                replicas = conn.execute(
                    """SELECT application_name, state, sync_state,
                              coalesce(pg_wal_lsn_diff(pg_current_wal_lsn(), replay_lsn), -1)::bigint
                         FROM pg_stat_replication"""
                ).fetchall()
                return {"reachable": True, "role": "primary", "sync_names": names,
                        "replicas": {r[0]: {"state": r[1], "sync_state": r[2], "lag": r[3]} for r in replicas}}
        except _Unreachable as exc:
            return {"reachable": False, "error": str(exc)}
        except psycopg.Error as exc:
            return {"reachable": False, "error": one_line(exc)}

    def _set_sync(self, node: str, value: str) -> bool:
        try:
            with self._connect(node) as conn, QueryWatchdog(conn, self._probe_timeout):
                conn.execute(sql.SQL("ALTER SYSTEM SET synchronous_standby_names = {}").format(sql.Literal(value)))
                conn.execute("SELECT pg_reload_conf()")
            return True
        except (_Unreachable, psycopg.Error) as exc:
            reason = str(exc) if isinstance(exc, _Unreachable) else one_line(exc)
            self._event(f"could not set synchronous_standby_names to '{value}' on {node}: {reason}")
            return False

    # ---- the loop -----------------------------------------------------------------------------

    def run(self, stop: threading.Event) -> None:
        self._event(f"watching {self._nodes[0]} and {self._nodes[1]} every {self._interval:g} s; "
                    f"failing over after {self._failures_needed} missed checks")
        while not stop.is_set():
            try:
                self._check()
            except Exception:
                # a bug in one check must not end failover protection for good
                log.exception("a monitor check failed unexpectedly")
                self._event("a monitor check failed unexpectedly; see the dbmonitor log")
            stop.wait(self._interval)

    def _check(self) -> None:
        if self._state_unsaved:
            self._save_state(self._designated)
        states = {node: self.probe(node) for node in self._nodes}
        primary = self._designated
        standby = self._other(primary)
        p, s = states[primary], states[standby]

        if p["reachable"] and p["role"] == "primary":
            if self._failures:
                self._event(f"{primary} answers again after {self._failures} missed checks")
            self._failures = 0
            self._resolved(f"down-{primary}", "standby-down-too", "standby-behind", "both-standby")

            replica = p["replicas"].get(standby)
            streaming = s["reachable"] and replica is not None and replica["state"] == "streaming"
            # recorded before the setting below changes, so a commit made without the standby is
            # never followed by a promotion of that standby
            self._standby_in_sync = streaming and replica["sync_state"] == "sync"
            wanted = "*" if streaming else ""
            if p["sync_names"] != wanted and self._set_sync(primary, wanted):
                if wanted:
                    self._event(f"{standby} is streaming, so commits on {primary} now wait until {standby} has applied them")
                else:
                    self._event(f"{standby} is not streaming, so {primary} commits without waiting for it")
                p["sync_names"] = wanted
            if s["reachable"] and s["role"] == "primary":
                self._event_once("two-primaries", f"{standby} runs as a primary next to {primary}; "
                                                  f"its fencing will stop it within a second")
            else:
                self._resolved("two-primaries")

        elif s["reachable"] and s["role"] == "primary":
            # S is a primary and P isn't (someone promoted it, or the monitor stopped between
            # promoting it and recording it). Record the primary that really exists.
            self._event(f"{standby} runs as the primary but the state file names {primary}; recording {standby}")
            self._adopt(standby)

        elif p["reachable"]:
            self._event_once("both-standby", f"{primary} is named the primary but runs as a standby, "
                                             f"and {standby} is not a primary either")
        else:
            self._event_once(f"down-{primary}", f"{primary} (the primary) is not answering: {p['error']}")
            if not s["reachable"]:
                # Nothing to fail over to. Missed checks only count while the standby is up, so
                # when both come back at once, the primary gets its full grace period to start.
                self._failures = 0
                self._event_once("standby-down-too", f"cannot fail over: the standby {standby} is not answering "
                                                 f"either ({s['error']})")
            else:
                self._resolved("standby-down-too")
                self._failures += 1
                if self._failures >= self._failures_needed:
                    self._fail_over(primary, standby)

        self._publish(states)

    def _fail_over(self, primary: str, standby: str) -> None:
        if not self._standby_in_sync:
            self._event_once("standby-behind",
                             f"not promoting {standby}: it was not a synchronous standby when {primary} was last "
                             f"seen, so it could be missing commits that users were told succeeded. Reads still "
                             f"work from {standby}; writes come back with {primary}.")
            return

        self._event(f"{primary} missed {self._failures} checks in a row; promoting {standby}")
        try:
            with self._connect(standby) as conn, QueryWatchdog(conn, 40):
                promoted = conn.execute("SELECT pg_promote(true, 30)").fetchone()[0]
        except _Unreachable as exc:
            self._event(f"promoting {standby} failed: {exc}")
            return
        except psycopg.Error as exc:
            self._event(f"promoting {standby} failed: {one_line(exc)}")
            return
        if not promoted:
            self._event(f"{standby} did not finish promoting within 30 s")
            return
        # It inherited "*" from its copy of the old primary's settings; with no standby of its
        # own, every commit would wait for ever.
        self._set_sync(standby, "")
        self._adopt(standby)
        self._event(f"{standby} is the primary now; {primary} will rejoin as its standby when it comes back")

    def _adopt(self, node: str) -> None:
        self._designated = node
        self._failures = 0
        self._standby_in_sync = False
        self._reported.clear()
        self._save_state(node)

    def _other(self, node: str) -> str:
        return self._nodes[1] if node == self._nodes[0] else self._nodes[0]

    # ---- status for the status page -----------------------------------------------------------

    def _publish(self, states: dict) -> None:
        primary = self._designated
        nodes = []
        for name in self._nodes:
            st = states[name]
            entry = {"name": name, "reachable": st["reachable"], "role": st.get("role", ""),
                     "error": st.get("error", ""), "replication": st.get("replication", ""), "lag_bytes": -1}
            if st.get("role") == "primary":
                replica = st["replicas"].get(self._other(name))
                if replica is None:
                    entry["replication"] = "no standby attached"
                else:
                    entry["replication"] = f"{self._other(name)} {replica['state']} ({replica['sync_state']})"
                    entry["lag_bytes"] = replica["lag"]
            nodes.append(entry)
        p = states.get(primary, {})
        with self._lock:
            self._status = {
                "designated_primary": primary,
                "nodes": nodes,
                "synchronous_standby_names": p.get("sync_names", "") if p.get("role") == "primary" else "",
                "standby_in_sync": self._standby_in_sync,
                "events": list(self._events),
                "checked_at": time.time(),
            }

    def status(self) -> dict | None:
        with self._lock:
            return self._status
