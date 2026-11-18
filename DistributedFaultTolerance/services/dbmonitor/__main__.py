import threading

import dbcluster_pb2_grpc
from common import config
from common.server import run_node

from .controller import DbClusterController
from .monitor import ClusterMonitor


def register(server, ctx):
    monitor = ClusterMonitor(
        config.host_list("DB_HOSTS", "db1,db2"),
        config.text("STATE_FILE", "/cluster-state/primary"),
        config.text("DB_SUPERUSER_PASSWORD"),
        interval=config.seconds("MONITOR_INTERVAL", 1.0, 0.2, 60),
        failures_needed=config.integer("MONITOR_FAILURES_BEFORE_FAILOVER", 3, 1, 100),
        probe_timeout=config.seconds("MONITOR_PROBE_TIMEOUT", 2.0, 0.5, 30),
    )
    # daemon: the loop holds nothing that needs closing when the node stops
    threading.Thread(target=monitor.run, args=(threading.Event(),), name="monitor", daemon=True).start()
    dbcluster_pb2_grpc.add_DbClusterServicer_to_server(DbClusterController(ctx, monitor), server)


run_node("dbmonitor", register, use_db=False)
