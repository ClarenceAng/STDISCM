"""The enrollment node's controller."""

import logging

import enrollment_pb2
import enrollment_pb2_grpc
from common import offerings
from common.errors import invalid
from common.rpc import handler

from . import model

log = logging.getLogger("enrollment")


def _offering_id(value: int) -> int:
    if value <= 0:
        raise invalid("Pick a section to enroll in.")
    return value


class EnrollmentController(enrollment_pb2_grpc.EnrollmentServiceServicer):
    def __init__(self, ctx):
        self.ctx = ctx

    @handler("student")
    def Enroll(self, request, context, user):
        course_code = request.course_code.strip().upper()
        section = request.section.strip().upper()
        if request.offering_id:
            offering_id = _offering_id(request.offering_id)
        elif course_code and section:
            if len(course_code) > 10 or len(section) > 4:
                raise invalid("A course code is at most 10 characters and a section at most 4.")
            offering_id = None
        else:
            raise invalid("Enter both a course code and a section, like STDISCM and S11.")
        row = self.ctx.db.write(lambda conn: model.enroll(conn, user.id, offering_id, course_code, section))
        log.info("%s enrolled in %s (%d of %d seats taken)", user.username, offerings.name(row), row["enrolled"], row["capacity"])
        return enrollment_pb2.EnrollReply(offering=offerings.to_offering(row, enrolled_by_me=True))

    @handler("student")
    def Drop(self, request, context, user):
        offering_id = _offering_id(request.offering_id)
        row = self.ctx.db.write(lambda conn: model.drop(conn, user.id, offering_id))
        log.info("%s dropped %s (%d of %d seats taken)", user.username, offerings.name(row), row["enrolled"], row["capacity"])
        return enrollment_pb2.DropReply(offering=offerings.to_offering(row, enrolled_by_me=False))

    @handler("student")
    def ListMyEnrollments(self, request, context, user):
        term, rows = self.ctx.db.read(lambda conn: model.my_enrollments(conn, user.id))
        return enrollment_pb2.ListMyEnrollmentsReply(
            term=offerings.to_term(term),
            offerings=[offerings.to_offering(r, enrolled_by_me=True) for r in rows],
            total_units=sum(r["units"] for r in rows),
        )
