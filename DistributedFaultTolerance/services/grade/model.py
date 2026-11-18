"""The grade node's model."""

from decimal import Decimal

from common import offerings
from common.errors import forbidden, not_found, refused

from .parsing import Entry, Problem, refuse


def grade_text(value: Decimal | None) -> str:
    return "" if value is None else f"{value:.1f}"


def gpa(graded: list[tuple[Decimal, int]]) -> str:
    """Weighted by units; "" when nothing is graded."""
    units = sum(u for _, u in graded)
    if units == 0:
        return ""
    return f"{sum(g * u for g, u in graded) / units:.3f}"


def my_grades(conn, student_id: int) -> list[dict]:
    """Every section the student finished or is taking, oldest term first, with its grade.
    The term open for enrollment is left out: it hasn't started."""
    return conn.execute(
        f"""SELECT {offerings.COLUMNS}, e.grade
              FROM {offerings.FROM}
              JOIN enrollments e ON e.offering_id = o.id
             WHERE e.student_id = %s AND e.status = 'enrolled' AND NOT t.enrollment_open
             ORDER BY t.sort_order, o.course_code""",
        (student_id,),
    ).fetchall()


def my_classes(conn, faculty_id: int) -> list[dict]:
    return conn.execute(
        f"""SELECT {offerings.COLUMNS},
                   (SELECT count(*) FROM enrollments e
                     WHERE e.offering_id = o.id AND e.status = 'enrolled' AND e.grade IS NOT NULL) AS graded
              FROM {offerings.FROM}
             WHERE o.faculty_id = %s
             ORDER BY t.sort_order DESC, o.course_code, o.section""",
        (faculty_id,),
    ).fetchall()


def _own_section(conn, faculty_id: int, offering_id: int, *, lock: bool) -> dict:
    # FOR SHARE: an admin can't hand the section to someone else while its grades are being saved
    row = conn.execute(
        f"SELECT {offerings.COLUMNS} FROM {offerings.FROM} WHERE o.id = %s" + (" FOR SHARE OF o" if lock else ""),
        (offering_id,),
    ).fetchone()
    if row is None:
        raise not_found(f"There is no section with id {offering_id}.")
    if row["faculty_id"] != faculty_id:
        teacher = f"; it is assigned to {row['faculty_name']}" if row["faculty_name"] else ""
        raise forbidden(f"You don't teach {offerings.name(row)}{teacher}.")
    return row


def roster(conn, faculty_id: int, offering_id: int) -> tuple[dict, list[dict]]:
    section = _own_section(conn, faculty_id, offering_id, lock=False)
    students = conn.execute(
        """SELECT u.id, u.username, u.full_name, u.role, e.grade
             FROM enrollments e JOIN users u ON u.id = e.student_id
            WHERE e.offering_id = %s AND e.status = 'enrolled'
            ORDER BY u.username""",
        (offering_id,),
    ).fetchall()
    return section, students


def upload(conn, faculty_id: int, offering_id: int, entries: list[Entry], problems: list[Problem]) -> tuple[int, int]:
    """Saves the grades that differ from what is stored. Returns (changed, unchanged).
    `problems` are the mistakes parsing found; nothing is saved if there are any, or if a student
    isn't in the class, and all of them are reported together."""
    section = _own_section(conn, faculty_id, offering_id, lock=True)
    name = offerings.name(section)
    if section["enrollment_open"]:
        raise refused(f"{name} is in {section['term_label']}, which hasn't started yet, so it can't be graded.")

    enrolled = {
        r["username"]: r for r in conn.execute(
            """SELECT e.id, u.username, e.grade
                 FROM enrollments e JOIN users u ON u.id = e.student_id
                WHERE e.offering_id = %s AND e.status = 'enrolled'
                  FOR UPDATE OF e""",
            (offering_id,),
        )
    }
    problems = problems + [Problem(e.order, f"{e.where}: {e.username} is not enrolled in {name}")
                           for e in entries if e.username not in enrolled]
    if problems:
        refuse(problems)

    changed = unchanged = 0
    for entry in entries:
        current = grade_text(enrolled[entry.username]["grade"])
        if entry.grade == "" or entry.grade == current:
            unchanged += 1
            continue
        conn.execute(
            "UPDATE enrollments SET grade = %s, graded_at = now(), graded_by = %s WHERE id = %s",
            (Decimal(entry.grade), faculty_id, enrolled[entry.username]["id"]),
        )
        changed += 1
    return changed, unchanged
