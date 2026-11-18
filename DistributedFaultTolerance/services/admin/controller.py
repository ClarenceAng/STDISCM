"""The admin node's controller."""

import logging
import re

import admin_pb2
import admin_pb2_grpc
import common_pb2
from common import offerings
from common.errors import invalid
from common.rpc import handler

from . import model

log = logging.getLogger("admin")

_COURSE_CODE = re.compile(r"^[A-Z][A-Z0-9]{2,9}$")
_SECTION = re.compile(r"^[A-Z0-9]{1,4}$")


def _text(value: str, label: str, longest: int) -> str:
    value = value.strip()
    if len(value) > longest:
        raise invalid(f"The {label} can be at most {longest} characters.")
    return value


def _capacity(value: int) -> int:
    if not 1 <= value <= 500:
        raise invalid("The capacity must be from 1 to 500.")
    return value


def _faculty(value: int) -> int:
    if value < 0:
        raise invalid("Pick a faculty member, or none.")
    return value


class AdminController(admin_pb2_grpc.AdminServiceServicer):
    def __init__(self, ctx):
        self.ctx = ctx

    @handler("admin")
    def ListTerms(self, request, context, user):
        rows = self.ctx.db.read(offerings.list_terms)
        return common_pb2.ListTermsReply(terms=[offerings.to_term(r) for r in rows])

    @handler("admin")
    def ListFaculty(self, request, context, user):
        rows = self.ctx.db.read(model.list_faculty)
        return admin_pb2.ListFacultyReply(faculty=[
            common_pb2.User(id=r["id"], username=r["username"], full_name=r["full_name"], role=r["role"]) for r in rows])

    @handler("admin")
    def ListOfferings(self, request, context, user):
        term_code = _text(request.term_code, "term code", 20)
        term, rows = self.ctx.db.read(lambda conn: model.list_offerings(conn, term_code))
        return admin_pb2.AdminListOfferingsReply(term=offerings.to_term(term),
                                                 offerings=[offerings.to_offering(r) for r in rows])

    @handler("admin")
    def CreateOffering(self, request, context, user):
        course_code = request.course_code.strip().upper()
        if not _COURSE_CODE.match(course_code):
            raise invalid("A course code is 3 to 10 capital letters and digits, starting with a letter, like STDISCM.")
        section = request.section.strip().upper()
        if not _SECTION.match(section):
            raise invalid("A section is 1 to 4 capital letters and digits, like S11.")
        if request.units and not 1 <= request.units <= 6:
            raise invalid("Units must be from 1 to 6.")
        term_code = _text(request.term_code, "term code", 20)
        if not term_code:
            raise invalid("Pick a term.")
        fields = dict(
            course_code=course_code,
            title=_text(request.title, "title", 120),
            units=request.units,
            term_code=term_code,
            section=section,
            schedule=_text(request.schedule, "schedule", 60),
            room=_text(request.room, "room", 30),
            faculty_id=_faculty(request.faculty_id),
            capacity=_capacity(request.capacity),
            is_open=request.open,
        )
        row = self.ctx.db.write(lambda conn: model.create_offering(conn, **fields))
        log.info("%s created %s in %s (capacity %d, %s)", user.username, offerings.name(row), row["term_code"],
                 row["capacity"], "open" if row["is_open"] else "closed")
        return offerings.to_offering(row)

    @handler("admin")
    def UpdateOffering(self, request, context, user):
        if request.offering_id <= 0:
            raise invalid("Pick a section.")
        fields = dict(offering_id=request.offering_id, capacity=_capacity(request.capacity),
                      is_open=request.open, faculty_id=_faculty(request.faculty_id))
        row = self.ctx.db.write(lambda conn: model.update_offering(conn, **fields))
        log.info("%s updated %s: capacity %d, %s, faculty %s", user.username, offerings.name(row),
                 row["capacity"], "open" if row["is_open"] else "closed", row["faculty_name"] or "none")
        return offerings.to_offering(row)
