"""The enrollment node's model.

Row-level locking: enroll and drop first lock the section's row with SELECT ... FOR UPDATE OF o.
A second request for the same section waits at that line until the first commits, and then reads
the seat count the first left behind. So checking for a free seat and taking it happen as one
step, and the last seat can't be given to two students. Requests for other sections lock other
rows and run side by side.

"OF o" matters: without it the query would also lock the joined terms row, and every enrollment
in the term would queue behind every other one.

Every transaction that locks both a section and enrollment rows locks the section first, so two
of them can't each hold what the other is waiting for (no deadlock).
"""

from common import offerings
from common.errors import conflict, not_found, refused

_LOCK_SECTION = """SELECT o.id, o.course_code, o.section, o.capacity, o.enrolled, o.is_open,
                          o.term_code, t.label AS term_label, t.enrollment_open
                     FROM offerings o JOIN terms t ON t.code = o.term_code
                    WHERE o.id = %s
                      FOR UPDATE OF o"""


def _lock_section(conn, offering_id: int) -> dict:
    row = conn.execute(_LOCK_SECTION, (offering_id,)).fetchone()
    if row is None:
        raise not_found(f"There is no section with id {offering_id}.")
    return row


def find_section(conn, course_code: str, section: str) -> int:
    """The id of a section of the term open for enrollment, by course code and section."""
    term = offerings.resolve_term(conn, "")
    row = conn.execute(
        "SELECT id FROM offerings WHERE term_code = %s AND course_code = %s AND section = %s",
        (term["code"], course_code, section),
    ).fetchone()
    if row is None:
        raise not_found(f"{term['label']} has no section {section} of {course_code}.")
    return row["id"]


def enroll(conn, student_id: int, offering_id: int | None, course_code: str = "", section_name: str = "") -> dict:
    if offering_id is None:
        offering_id = find_section(conn, course_code, section_name)
    section = _lock_section(conn, offering_id)
    name = offerings.name(section)
    if not section["enrollment_open"]:
        raise refused(f"Enrollment for {section['term_label']} is not open, so you can't enroll in {name}.")
    if not section["is_open"]:
        raise refused(f"{name} is closed for enrollment.")

    mine = conn.execute(
        "SELECT status FROM enrollments WHERE offering_id = %s AND student_id = %s FOR UPDATE",
        (offering_id, student_id),
    ).fetchone()
    if mine is not None and mine["status"] == "enrolled":
        raise conflict(f"You are already enrolled in {name}.")

    other = conn.execute(
        """SELECT o.section FROM enrollments e JOIN offerings o ON o.id = e.offering_id
            WHERE e.student_id = %s AND e.status = 'enrolled'
              AND o.term_code = %s AND o.course_code = %s AND o.id <> %s""",
        (student_id, section["term_code"], section["course_code"], offering_id),
    ).fetchone()
    if other is not None:
        raise refused(f"You are already enrolled in {section['course_code']} {other['section']}. "
                      "Drop it before taking another section.")

    if section["enrolled"] >= section["capacity"]:
        taken = "its only seat is" if section["capacity"] == 1 else f"all {section['capacity']} seats are"
        raise refused(f"{name} is full: {taken} taken.")

    conn.execute(
        """INSERT INTO enrollments (offering_id, student_id, status) VALUES (%s, %s, 'enrolled')
           ON CONFLICT (offering_id, student_id)
           DO UPDATE SET status = 'enrolled', enrolled_at = now(), dropped_at = NULL""",
        (offering_id, student_id),
    )
    conn.execute("UPDATE offerings SET enrolled = enrolled + 1 WHERE id = %s", (offering_id,))
    return offerings.fetch(conn, offering_id)


def drop(conn, student_id: int, offering_id: int) -> dict:
    section = _lock_section(conn, offering_id)
    name = offerings.name(section)
    if not section["enrollment_open"]:
        raise refused(f"{section['term_label']} is no longer open for changes, so {name} can't be dropped.")

    mine = conn.execute(
        "SELECT status, grade FROM enrollments WHERE offering_id = %s AND student_id = %s FOR UPDATE",
        (offering_id, student_id),
    ).fetchone()
    if mine is None or mine["status"] != "enrolled":
        raise refused(f"You are not enrolled in {name}.")
    if mine["grade"] is not None:
        raise refused(f"{name} already has a grade, so it can't be dropped.")

    conn.execute(
        "UPDATE enrollments SET status = 'dropped', dropped_at = now() WHERE offering_id = %s AND student_id = %s",
        (offering_id, student_id),
    )
    conn.execute("UPDATE offerings SET enrolled = enrolled - 1 WHERE id = %s", (offering_id,))
    return offerings.fetch(conn, offering_id)


def my_enrollments(conn, student_id: int) -> tuple[dict, list[dict]]:
    term = offerings.resolve_term(conn, "")
    rows = conn.execute(
        f"""SELECT {offerings.COLUMNS} FROM {offerings.FROM}
              JOIN enrollments e ON e.offering_id = o.id
             WHERE e.student_id = %s AND e.status = 'enrolled' AND o.term_code = %s
             ORDER BY o.course_code, o.section""",
        (student_id, term["code"]),
    ).fetchall()
    return term, rows
