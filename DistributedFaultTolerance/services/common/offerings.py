"""SQL and message conversion for terms and sections, used by the models of several nodes."""

import common_pb2

from .errors import not_found

# Columns and joins for one section with its course, term and faculty. Queries add their own
# WHERE, and FOR UPDATE OF o / FOR SHARE OF o so that only the section row is locked.
COLUMNS = """o.id, o.course_code, c.title, c.units,
             t.code AS term_code, t.label AS term_label, t.enrollment_open,
             o.section, o.schedule, o.room, o.faculty_id, coalesce(f.full_name, '') AS faculty_name,
             o.capacity, o.enrolled, o.is_open"""
FROM = """offerings o
          JOIN courses c ON c.code = o.course_code
          JOIN terms t ON t.code = o.term_code
          LEFT JOIN users f ON f.id = o.faculty_id"""


def to_term(row: dict, prefix: str = "") -> common_pb2.Term:
    return common_pb2.Term(code=row[f"{prefix}code"], label=row[f"{prefix}label"],
                           enrollment_open=row["enrollment_open"])


def to_offering(row: dict, enrolled_by_me: bool = False) -> common_pb2.Offering:
    return common_pb2.Offering(
        id=row["id"],
        course_code=row["course_code"],
        title=row["title"],
        units=row["units"],
        term=to_term(row, "term_"),
        section=row["section"],
        schedule=row["schedule"],
        room=row["room"],
        faculty_id=row["faculty_id"] or 0,
        faculty_name=row["faculty_name"],
        capacity=row["capacity"],
        enrolled=row["enrolled"],
        open=row["is_open"],
        enrolled_by_me=enrolled_by_me,
    )


def fetch(conn, offering_id: int) -> dict | None:
    return conn.execute(f"SELECT {COLUMNS} FROM {FROM} WHERE o.id = %s", (offering_id,)).fetchone()


def list_terms(conn) -> list[dict]:
    return conn.execute("SELECT code, label, enrollment_open FROM terms ORDER BY sort_order").fetchall()


def resolve_term(conn, code: str) -> dict:
    """The term with this code, or the term open for enrollment when the code is empty."""
    if code:
        row = conn.execute("SELECT code, label, enrollment_open FROM terms WHERE code = %s", (code,)).fetchone()
        if row is None:
            raise not_found(f"There is no term {code}.")
        return row
    row = conn.execute("SELECT code, label, enrollment_open FROM terms WHERE enrollment_open").fetchone()
    if row is None:
        raise not_found("No term is open for enrollment right now.")
    return row


def name(row: dict) -> str:
    return f"{row['course_code']} {row['section']}"
