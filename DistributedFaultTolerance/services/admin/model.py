"""The admin node's model."""

from psycopg import errors as pg_errors

from common import offerings
from common.errors import conflict, invalid, not_found, refused


def list_faculty(conn) -> list[dict]:
    return conn.execute(
        "SELECT id, username, full_name, role FROM users WHERE role = 'faculty' ORDER BY full_name"
    ).fetchall()


def list_offerings(conn, term_code: str) -> tuple[dict, list[dict]]:
    term = offerings.resolve_term(conn, term_code)
    rows = conn.execute(
        f"SELECT {offerings.COLUMNS} FROM {offerings.FROM} WHERE o.term_code = %s ORDER BY o.course_code, o.section",
        (term["code"],),
    ).fetchall()
    return term, rows


def _check_faculty(conn, faculty_id: int) -> None:
    if faculty_id == 0:
        return
    row = conn.execute("SELECT role, full_name FROM users WHERE id = %s", (faculty_id,)).fetchone()
    if row is None:
        raise not_found(f"There is no user with id {faculty_id}.")
    if row["role"] != "faculty":
        raise invalid(f"{row['full_name']} is not a faculty member.")


def create_offering(conn, *, course_code: str, title: str, units: int, term_code: str, section: str,
                    schedule: str, room: str, faculty_id: int, capacity: int, is_open: bool) -> dict:
    term = conn.execute("SELECT code, label FROM terms WHERE code = %s", (term_code,)).fetchone()
    if term is None:
        raise not_found(f"There is no term {term_code}.")

    course = conn.execute("SELECT code, title, units FROM courses WHERE code = %s", (course_code,)).fetchone()
    if course is None:
        if not title or units == 0:
            raise invalid(f"{course_code} is a new course, so it needs a title and units.")
        # ON CONFLICT: another admin may be adding the same new course at this moment
        conn.execute("INSERT INTO courses (code, title, units) VALUES (%s, %s, %s) ON CONFLICT (code) DO NOTHING",
                     (course_code, title, units))
        course = conn.execute("SELECT code, title, units FROM courses WHERE code = %s", (course_code,)).fetchone()
    if (title and title != course["title"]) or (units and units != course["units"]):
        raise refused(f"{course_code} already exists as \"{course['title']}\" ({course['units']} units). "
                      "Leave the title and units empty to add a section of it.")

    _check_faculty(conn, faculty_id)
    try:
        new_id = conn.execute(
            """INSERT INTO offerings (course_code, term_code, section, schedule, room, faculty_id, capacity, is_open)
               VALUES (%s, %s, %s, %s, %s, %s, %s, %s) RETURNING id""",
            (course_code, term_code, section, schedule, room, faculty_id or None, capacity, is_open),
        ).fetchone()["id"]
    except pg_errors.UniqueViolation:
        raise conflict(f"{course_code} already has a section {section} in {term['label']}.") from None
    return offerings.fetch(conn, new_id)


def update_offering(conn, *, offering_id: int, capacity: int, is_open: bool, faculty_id: int) -> dict:
    # Row lock, like enrolling: a capacity cut and an enrollment in the same section take turns,
    # so the capacity can't drop below a seat count that is changing underneath it.
    row = conn.execute(
        "SELECT id, course_code, section, enrolled FROM offerings WHERE id = %s FOR UPDATE",
        (offering_id,),
    ).fetchone()
    if row is None:
        raise not_found(f"There is no section with id {offering_id}.")
    if capacity < row["enrolled"]:
        raise refused(f"{offerings.name(row)} already has {row['enrolled']} students, "
                      f"so its capacity can't go below {row['enrolled']}.")
    _check_faculty(conn, faculty_id)
    conn.execute("UPDATE offerings SET capacity = %s, is_open = %s, faculty_id = %s WHERE id = %s",
                 (capacity, is_open, faculty_id or None, offering_id))
    return offerings.fetch(conn, offering_id)
