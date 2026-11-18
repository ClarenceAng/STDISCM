import enrollment_pb2_grpc
from common.server import run_node

from .controller import EnrollmentController


def register(server, ctx):
    enrollment_pb2_grpc.add_EnrollmentServiceServicer_to_server(EnrollmentController(ctx), server)


run_node("enrollment", register)
