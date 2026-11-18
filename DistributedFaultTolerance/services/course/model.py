"""The course node's model. Read only."""

from common import offerings


def list_terms(conn) -> list[dict]:
    return offerings.list_terms(conn)


def list_offerings(conn, term_code: str, student_id: int | None) -> tuple[dict, list[dict], set[int]]:
    """The sections of a term, and which of them the student is enrolled in."""
    term = offerings.resolve_term(conn, term_code)
    rows = conn.execute(
        f"SELECT {offerings.COLUMNS} FROM {offerings.FROM} WHERE o.term_code = %s ORDER BY o.course_code, o.section",
        (term["code"],),
    ).fetchall()
    mine: set[int] = set()
    if student_id is not None:
        mine = {r["offering_id"] for r in conn.execute(
            """SELECT e.offering_id FROM enrollments e JOIN offerings o ON o.id = e.offering_id
                WHERE e.student_id = %s AND e.status = 'enrolled' AND o.term_code = %s""",
            (student_id, term["code"]),
        )}
    return term, rows, mine
