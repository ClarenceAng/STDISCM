import course_pb2_grpc
from common.server import run_node

from .controller import CourseController


def register(server, ctx):
    course_pb2_grpc.add_CourseServiceServicer_to_server(CourseController(ctx), server)


run_node("course", register)
