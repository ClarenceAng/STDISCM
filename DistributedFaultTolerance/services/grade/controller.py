"""The grade node's controller."""

import logging
from itertools import groupby

import common_pb2
import grade_pb2
import grade_pb2_grpc
from common import offerings
from common.errors import invalid
from common.rpc import handler

from . import model, parsing

log = logging.getLogger("grade")


class GradeController(grade_pb2_grpc.GradeServiceServicer):
    def __init__(self, ctx):
        self.ctx = ctx

    @handler("student")
    def ListMyGrades(self, request, context, user):
        rows = self.ctx.db.read(lambda conn: model.my_grades(conn, user.id))
        terms = []
        everything = []
        for _, term_rows in groupby(rows, key=lambda r: r["term_code"]):
            term_rows = list(term_rows)
            graded = [(r["grade"], r["units"]) for r in term_rows if r["grade"] is not None]
            everything.extend(graded)
            terms.append(grade_pb2.TermGrades(
                term=offerings.to_term(term_rows[0], "term_"),
                courses=[grade_pb2.GradedCourse(offering=offerings.to_offering(r, enrolled_by_me=True),
                                                grade=model.grade_text(r["grade"])) for r in term_rows],
                gpa=model.gpa(graded),
                graded_units=sum(u for _, u in graded),
            ))
        return grade_pb2.ListMyGradesReply(
            terms=terms,
            cumulative_gpa=model.gpa(everything),
            graded_units=sum(u for _, u in everything),
        )

    @handler("faculty")
    def ListMyClasses(self, request, context, user):
        rows = self.ctx.db.read(lambda conn: model.my_classes(conn, user.id))
        return grade_pb2.ListMyClassesReply(
            classes=[grade_pb2.ClassSummary(offering=offerings.to_offering(r), graded=r["graded"]) for r in rows])

    @handler("faculty")
    def GetClassRoster(self, request, context, user):
        if request.offering_id <= 0:
            raise invalid("Pick a section.")
        section, students = self.ctx.db.read(lambda conn: model.roster(conn, user.id, request.offering_id))
        return grade_pb2.GetClassRosterReply(
            offering=offerings.to_offering(section),
            students=[grade_pb2.RosterEntry(
                student=common_pb2.User(id=s["id"], username=s["username"], full_name=s["full_name"], role=s["role"]),
                grade=model.grade_text(s["grade"])) for s in students],
        )

    @handler("faculty")
    def UploadGrades(self, request, context, user):
        if request.offering_id <= 0:
            raise invalid("Pick a section.")
        if request.csv and request.entries:
            raise invalid("Send either the form or a CSV file, not both.")
        if request.csv:
            entries, problems = parsing.from_csv(request.csv)
            source = "CSV file"
        elif request.entries:
            entries, problems = parsing.from_form(request.entries)
            source = "form"
        else:
            raise invalid("No grades were sent.")

        changed, unchanged = self.ctx.db.write(
            lambda conn: model.upload(conn, user.id, request.offering_id, entries, problems))
        log.info("%s uploaded grades for section %d from a %s: %d changed, %d unchanged",
                 user.username, request.offering_id, source, changed, unchanged)
        return grade_pb2.UploadGradesReply(changed=changed, unchanged=unchanged)
