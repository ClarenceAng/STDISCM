import grade_pb2_grpc
from common.server import run_node

from .controller import GradeController


def register(server, ctx):
    grade_pb2_grpc.add_GradeServiceServicer_to_server(GradeController(ctx), server)


run_node("grade", register)
