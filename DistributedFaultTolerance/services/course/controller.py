"""The course node's controller."""

import common_pb2
import course_pb2
import course_pb2_grpc
from common import offerings
from common.errors import invalid
from common.rpc import handler

from . import model


class CourseController(course_pb2_grpc.CourseServiceServicer):
    def __init__(self, ctx):
        self.ctx = ctx

    @handler()   # any signed-in user
    def ListTerms(self, request, context, user):
        rows = self.ctx.db.read(model.list_terms)
        return common_pb2.ListTermsReply(terms=[offerings.to_term(r) for r in rows])

    @handler()
    def ListOfferings(self, request, context, user):
        term_code = request.term_code.strip()
        if len(term_code) > 20:
            raise invalid("The term code is too long.")
        student_id = user.id if user.role == "student" else None
        term, rows, mine = self.ctx.db.read(lambda conn: model.list_offerings(conn, term_code, student_id))
        return course_pb2.ListOfferingsReply(
            term=offerings.to_term(term),
            offerings=[offerings.to_offering(r, r["id"] in mine) for r in rows],
        )
