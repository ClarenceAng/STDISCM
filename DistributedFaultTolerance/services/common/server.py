"""Starts a backend node: its gRPC server with its controller, the standard health service the
status page asks, and NodeInfo with the node's pool statistics."""

import logging
import signal
import sys
import threading
import time
from concurrent import futures
from dataclasses import dataclass
from typing import Callable, NoReturn

import grpc
from grpc_health.v1 import health, health_pb2, health_pb2_grpc

import node_pb2
import node_pb2_grpc

from . import config, logs, tls
from .config import ConfigError
from .db import Database
from .sessions import SessionVerifier


@dataclass
class NodeContext:
    node: str
    db: Database | None
    verifier: SessionVerifier | None
    started_at: float


class NodeInfoController(node_pb2_grpc.NodeInfoServicer):
    def __init__(self, ctx: NodeContext):
        self.ctx = ctx

    def GetInfo(self, request, context):
        pools = [] if self.ctx.db is None else [self.ctx.db.primary.stats(), self.ctx.db.standby.stats()]
        return node_pb2.GetInfoReply(
            node=self.ctx.node,
            started_at=int(self.ctx.started_at),
            pools=[node_pb2.PoolStats(**stats) for stats in pools],
        )


def run_node(node: str, register: Callable[[grpc.Server, NodeContext], None], *, use_db: bool = True) -> NoReturn:
    """`register` adds the node's controllers to the server. It may raise ConfigError."""
    logs.setup(node)
    log = logging.getLogger("node")

    try:
        port = config.integer("GRPC_PORT", 50051, 1, 65535)
        workers = config.integer("GRPC_WORKERS", 32, 1, 512)
        credentials = tls.server_credentials()
        db = verifier = None
        if use_db:
            db = Database(node)
            key_path = config.text("PUBLIC_KEY_PATH")
            verifier = SessionVerifier(config.read_file("PUBLIC_KEY_PATH", key_path), db)
        ctx = NodeContext(node=node, db=db, verifier=verifier, started_at=time.time())

        server = grpc.server(
            futures.ThreadPoolExecutor(max_workers=workers, thread_name_prefix=node),
            options=[
                # ping idle callers so a vanished view node's connection is noticed and closed
                ("grpc.keepalive_time_ms", 20000),
                ("grpc.keepalive_timeout_ms", 5000),
                ("grpc.keepalive_permit_without_calls", 1),
                ("grpc.http2.min_ping_interval_without_data_ms", 5000),
            ],
        )
        register(server, ctx)
    except ConfigError as exc:
        log.error("cannot start: %s", exc)
        sys.exit(2)

    health_service = health.HealthServicer()
    health_pb2_grpc.add_HealthServicer_to_server(health_service, server)
    node_pb2_grpc.add_NodeInfoServicer_to_server(NodeInfoController(ctx), server)

    # Callers check the certificate against mkcert's certificate authority and that it is for this
    # node's name. The pair was checked in tls.server_credentials, so a failure here is the port.
    try:
        bound = server.add_secure_port(f"0.0.0.0:{port}", credentials)
    except RuntimeError as exc:
        log.error("cannot listen with TLS on port %d: %s", port, exc)
        sys.exit(1)
    if bound == 0:
        log.error("cannot listen with TLS on port %d (is it in use?)", port)
        sys.exit(1)

    server.start()
    health_service.set("", health_pb2.HealthCheckResponse.SERVING)
    log.info("listening for gRPC with TLS on port %d with %d worker threads", port, workers)

    stop = threading.Event()

    def on_signal(signum, _frame):
        log.info("got %s; finishing the requests in progress (up to 5 s)", signal.Signals(signum).name)
        stop.set()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    while not stop.wait(1):
        pass

    health_service.enter_graceful_shutdown()
    server.stop(grace=5).wait()
    if db is not None:
        db.close()
    log.info("stopped")
    sys.exit(0)
