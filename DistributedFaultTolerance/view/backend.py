"""The view node's gRPC clients for the other nodes.

Every call returns a Result instead of raising. When a node is down, the page that needs it shows
why, and the rest of the page (and every other page) still works.
"""

import logging
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any

import grpc
from grpc_health.v1 import health_pb2, health_pb2_grpc

import admin_pb2_grpc
import auth_pb2_grpc
import course_pb2_grpc
import dbcluster_pb2_grpc
import enrollment_pb2_grpc
import grade_pb2_grpc
import node_pb2
import node_pb2_grpc

log = logging.getLogger("backend")


@dataclass(frozen=True)
class NodeSpec:
    name: str
    label: str
    env: str
    feature: str      # what stops working without it, used inside sentences
    stub: Any


NODES = {
    "auth": NodeSpec("auth", "Auth", "AUTH_ADDR", "signing in and out", auth_pb2_grpc.AuthServiceStub),
    "course": NodeSpec("course", "Course", "COURSE_ADDR", "viewing courses", course_pb2_grpc.CourseServiceStub),
    "enrollment": NodeSpec("enrollment", "Enrollment", "ENROLLMENT_ADDR", "enrolling and dropping",
                           enrollment_pb2_grpc.EnrollmentServiceStub),
    "grade": NodeSpec("grade", "Grade", "GRADE_ADDR", "viewing and uploading grades", grade_pb2_grpc.GradeServiceStub),
    "admin": NodeSpec("admin", "Admin", "ADMIN_ADDR", "managing sections", admin_pb2_grpc.AdminServiceStub),
    "dbmonitor": NodeSpec("dbmonitor", "Database monitor", "DBMONITOR_ADDR", "database failover",
                          dbcluster_pb2_grpc.DbClusterStub),
}

# A node whose connection dropped is retried at most every 2 s, instead of gRPC's default back-off
# that grows to 2 minutes. (A node whose name stopped resolving is handled by replacing the
# channel; see Backend._replace_channel.)
CHANNEL_OPTIONS = [
    ("grpc.initial_reconnect_backoff_ms", 500),
    ("grpc.min_reconnect_backoff_ms", 500),
    ("grpc.max_reconnect_backoff_ms", 2000),
    ("grpc.keepalive_time_ms", 20000),
    ("grpc.keepalive_timeout_ms", 5000),
    ("grpc.keepalive_permit_without_calls", 1),
]


# Calls that change something, for which a timeout leaves the user unsure whether it happened
CHANGES = {"Enroll", "Drop", "UploadGrades", "CreateOffering", "UpdateOffering", "Logout"}


@dataclass
class Failure:
    node: str          # label of the node that failed
    kind: str          # down, timeout, database, busy, refused, signed_out, forbidden, failed
    message: str       # for the user
    detail: str = ""   # gRPC's own words, shown smaller


@dataclass
class Result:
    reply: Any = None
    failure: Failure | None = None
    from_standby: bool = False   # the node read this from the standby database

    @property
    def ok(self) -> bool:
        return self.failure is None


@dataclass
class Health:
    node: NodeSpec
    address: str
    state: str         # up, not serving, down, timeout, error
    detail: str
    ms: float


@dataclass
class _Connection:
    channel: grpc.Channel
    stub: Any
    health: health_pb2_grpc.HealthStub
    info: node_pb2_grpc.NodeInfoStub


class Backend:
    PROBE_EVERY = 1.0      # seconds between attempts to reach a node that went away
    PROBE_TIMEOUT = 6.0    # covers Docker's DNS, which takes about 4 s to say a name is gone

    def __init__(self, addresses: dict[str, str], timeout: float):
        self.addresses = addresses
        self.timeout = timeout
        self._lock = threading.Lock()
        self._connections = {name: self._connect(name) for name in addresses}
        self._reviving: set[str] = set()
        self._pool = ThreadPoolExecutor(max_workers=2 * len(addresses), thread_name_prefix="status")
        # Connect every channel now, in the background. A channel's first name lookup takes Docker
        # about 4 s when the node is gone; done now, no page waits for it later, and a node that is
        # down already is handed to its background reviver.
        for name in addresses:
            self._pool.submit(self.health, name, 6.0)

    def _connect(self, node: str) -> _Connection:
        channel = grpc.insecure_channel(self.addresses[node], options=CHANNEL_OPTIONS)
        return _Connection(channel, NODES[node].stub(channel), health_pb2_grpc.HealthStub(channel),
                           node_pb2_grpc.NodeInfoStub(channel))

    def _get(self, node: str) -> _Connection:
        with self._lock:
            return self._connections[node]

    def _replace_channel(self, node: str, failed: _Connection) -> None:
        """Called after a call to `node` came back UNAVAILABLE.

        When a container stops, Docker's DNS stops resolving its name, and gRPC's resolver then
        retries the lookup on its own back-off of up to 2 minutes, which no channel option
        shortens. So the node would stay unusable long after it is back.

        A new channel looks the name up at once, but each lookup of a missing name takes Docker
        about 4 s, so requests must not wait for one. Requests keep the failed channel, which
        fails at once, while one background thread per node tries a new channel every second and
        swaps it in when the node answers a health check.
        """
        with self._lock:
            if node in self._reviving or self._connections[node] is not failed:
                return
            self._reviving.add(node)
        threading.Thread(target=self._revive, args=(node, failed), name=f"revive-{node}", daemon=True).start()

    def _revive(self, node: str, failed: _Connection) -> None:
        log.info("the %s node is unavailable; looking for it every %g s", node, self.PROBE_EVERY)
        attempts = 0
        try:
            while True:
                attempts += 1
                fresh = self._connect(node)
                try:
                    fresh.health.Check(health_pb2.HealthCheckRequest(service=""), timeout=self.PROBE_TIMEOUT)
                    break
                except grpc.RpcError as exc:
                    if attempts == 1 or attempts % 30 == 0:
                        log.info("the %s node is still unavailable after %d tries: %s", node,
                                 attempts, (exc.details() or exc.code().name).splitlines()[0][:200])
                    fresh.channel.close()
                with self._lock:
                    if self._connections[node] is not failed:
                        return   # something else replaced it already
                time.sleep(self.PROBE_EVERY)
            with self._lock:
                # The old channel is not closed: requests may still hold it. It is freed when they finish.
                self._connections[node] = fresh
            log.info("the %s node answers again (after %d tries); using a new channel", node, attempts)
        finally:
            with self._lock:
                self._reviving.discard(node)

    def call(self, node: str, method: str, request, token: str | None = None, timeout: float | None = None) -> Result:
        spec = NODES[node]
        metadata = (("authorization", f"Bearer {token}"),) if token else None
        limit = timeout or self.timeout
        started = time.monotonic()
        conn = self._get(node)
        for attempt in (1, 2):
            try:
                reply, call = getattr(conn.stub, method).with_call(
                    request, metadata=metadata, timeout=max(0.1, limit - (time.monotonic() - started)))
            except grpc.RpcError as exc:
                if attempt == 1 and _never_arrived(exc) and not self._known_down(node):
                    fresh = self._try_fresh_channel(node, conn, limit - (time.monotonic() - started))
                    if fresh is not None:
                        log.info("%s.%s: the old channel led nowhere (%s); retrying on a new one",
                                 node, method, (exc.details() or "").splitlines()[0][:160])
                        conn = fresh
                        continue
                if exc.code() == grpc.StatusCode.UNAVAILABLE:
                    self._replace_channel(node, conn)
                failure = describe(spec, method, exc, limit)
                log.info("%s.%s failed: %s (%s)", node, method, failure.kind, failure.detail or failure.message)
                return Result(failure=failure)
            trailers = dict(call.trailing_metadata() or ())
            return Result(reply=reply, from_standby=trailers.get("db-source") == "standby")

    def _known_down(self, node: str) -> bool:
        """A background thread is already looking for this node, so requests should fail at once."""
        with self._lock:
            return node in self._reviving

    def _try_fresh_channel(self, node: str, failed: _Connection, time_left: float) -> _Connection | None:
        """A node that was recreated usually has a new address, and the old channel still points at
        the old one. A new channel looks the name up again; if the node answers on it at once, it
        replaces the old channel. A node that is up answers in milliseconds, so the check waits at
        most 0.5 s; once a node is known to be down this isn't tried at all."""
        if time_left < 0.8:
            return None
        fresh = self._connect(node)
        try:
            fresh.health.Check(health_pb2.HealthCheckRequest(service=""), timeout=0.5)
        except grpc.RpcError:
            fresh.channel.close()
            return None
        with self._lock:
            if self._connections[node] is failed:
                self._connections[node] = fresh
                return fresh
            current = self._connections[node]
        fresh.channel.close()
        return current

    def health(self, node: str, timeout: float = 1.5) -> Health:
        spec = NODES[node]
        started = time.monotonic()
        conn = self._get(node)
        try:
            reply = conn.health.Check(health_pb2.HealthCheckRequest(service=""), timeout=timeout)
            state = "up" if reply.status == health_pb2.HealthCheckResponse.SERVING else "not serving"
            detail = health_pb2.HealthCheckResponse.ServingStatus.Name(reply.status)
        except grpc.RpcError as exc:
            code = exc.code()
            if code == grpc.StatusCode.UNAVAILABLE:
                self._replace_channel(node, conn)
            state = {grpc.StatusCode.UNAVAILABLE: "down", grpc.StatusCode.DEADLINE_EXCEEDED: "timeout"}.get(code, "error")
            detail = f"{code.name}: {exc.details()}"
        return Health(spec, self.addresses[node], state, detail, (time.monotonic() - started) * 1000)

    def info(self, node: str, timeout: float = 1.5) -> Result:
        conn = self._get(node)
        try:
            return Result(reply=conn.info.GetInfo(node_pb2.GetInfoRequest(), timeout=timeout))
        except grpc.RpcError as exc:
            if exc.code() == grpc.StatusCode.UNAVAILABLE:
                self._replace_channel(node, conn)
            return Result(failure=describe(NODES[node], "GetInfo", exc, timeout))

    def in_parallel(self, *calls):
        """Runs (function, args...) tuples at once, so a status page waits for the slowest node only."""
        futures = [self._pool.submit(fn, *args) for fn, *args in calls]
        return [f.result() for f in futures]


def _never_arrived(exc: grpc.RpcError) -> bool:
    """True when gRPC couldn't even connect, so the node never saw the request and sending it again
    can't do anything twice. A node reports its own trouble with error-origin=database, and a node
    whose name doesn't resolve is gone, so neither of those is retried."""
    if exc.code() != grpc.StatusCode.UNAVAILABLE:
        return False
    if dict(exc.trailing_metadata() or ()).get("error-origin"):
        return False
    details = exc.details() or ""
    # "Socket closed" is not here: the connection can close after the request went out.
    return "DNS resolution failed" not in details and (
        "failed to connect" in details or "Connection refused" in details)


def describe(spec: NodeSpec, method: str, exc: grpc.RpcError, timeout: float) -> Failure:
    code = exc.code()
    details = exc.details() or ""
    trailers = dict(exc.trailing_metadata() or ())
    label = spec.label

    if trailers.get("error-origin") == "database":
        if code == grpc.StatusCode.RESOURCE_EXHAUSTED:
            return Failure(label, "busy", details)
        if code == grpc.StatusCode.UNKNOWN:
            return Failure(label, "database", details)   # the commit may or may not have happened
        return Failure(label, "database",
                       f"The {label} node is running, but it can't use the database right now, "
                       f"so {spec.feature} is unavailable.", details)
    if code == grpc.StatusCode.UNAVAILABLE:
        return Failure(label, "down",
                       f"The {label} node can't be reached, so {spec.feature} is unavailable right now. "
                       "Everything else still works.", details)
    if code == grpc.StatusCode.DEADLINE_EXCEEDED:
        caution = (" The node rolls back a change it couldn't finish in time, but check before trying again."
                   if method in CHANGES else "")
        return Failure(label, "timeout",
                       f"The {label} node didn't answer within {timeout:g} s, so {spec.feature} "
                       f"is unavailable right now. Everything else still works.{caution}", details)
    if code == grpc.StatusCode.UNAUTHENTICATED:
        return Failure(label, "signed_out", details)
    if code == grpc.StatusCode.PERMISSION_DENIED:
        return Failure(label, "forbidden", details)
    if code in (grpc.StatusCode.INVALID_ARGUMENT, grpc.StatusCode.FAILED_PRECONDITION, grpc.StatusCode.NOT_FOUND,
                grpc.StatusCode.ALREADY_EXISTS, grpc.StatusCode.ABORTED, grpc.StatusCode.OUT_OF_RANGE,
                grpc.StatusCode.RESOURCE_EXHAUSTED):
        return Failure(label, "refused", details)
    if code == grpc.StatusCode.UNIMPLEMENTED:
        return Failure(label, "failed", f"The {label} node doesn't know {method}. Is it running an older version?", details)
    if code == grpc.StatusCode.CANCELLED:
        return Failure(label, "failed", f"The request to the {label} node was cancelled.", details)
    return Failure(label, "failed", f"The {label} node failed: {details}", code.name)
