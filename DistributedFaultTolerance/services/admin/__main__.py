import admin_pb2_grpc
from common.server import run_node

from .controller import AdminController


def register(server, ctx):
    admin_pb2_grpc.add_AdminServiceServicer_to_server(AdminController(ctx), server)


run_node("admin", register)
