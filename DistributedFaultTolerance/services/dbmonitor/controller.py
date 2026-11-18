"""The dbmonitor node's controller: the cluster status for the status page."""

import dbcluster_pb2
import dbcluster_pb2_grpc
from common.errors import refused
from common.rpc import handler


class DbClusterController(dbcluster_pb2_grpc.DbClusterServicer):
    def __init__(self, ctx, monitor):
        self.ctx = ctx
        self._monitor = monitor

    @handler(public=True)
    def GetStatus(self, request, context):
        status = self._monitor.status()
        if status is None:
            raise refused("The monitor has not finished its first check of the database nodes yet.")
        return dbcluster_pb2.ClusterStatus(
            designated_primary=status["designated_primary"],
            nodes=[dbcluster_pb2.DbNode(**n) for n in status["nodes"]],
            synchronous_standby_names=status["synchronous_standby_names"],
            standby_in_sync=status["standby_in_sync"],
            events=[dbcluster_pb2.ClusterEvent(at=int(at), message=msg) for at, msg in status["events"]],
            checked_at=int(status["checked_at"]),
        )
