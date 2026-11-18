"""Checks the grades a faculty member sends, from the web form or a CSV file, before any are saved.

Problems are collected rather than raised one at a time, so the faculty member gets every mistake
in the file in one answer. The model adds the students who aren't in the class, then refuses the
upload if there is anything to fix.
"""

import csv
import io
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation

from common.errors import invalid

GRADES = ("4.0", "3.5", "3.0", "2.5", "2.0", "1.5", "1.0", "0.0")
_GRADE_LIST = ", ".join(GRADES[:-1]) + " or " + GRADES[-1]
MAX_CSV_BYTES = 1_000_000
MAX_LINES = 2000
_HEADER_WORDS = {"username", "user", "student", "student username"}


@dataclass(frozen=True)
class Entry:
    order: int        # line number, or position in the form, for sorting problems
    where: str        # "line 4" for a CSV line, the username for a form row
    username: str
    grade: str        # one of GRADES, or "" to leave the grade as it is


@dataclass(frozen=True)
class Problem:
    order: int
    text: str


def normalize_grade(raw: str) -> str | None:
    """'4', '4.0' and '4.00' all become '4.0'. None if it isn't a grade on the scale."""
    text = raw.strip()
    if text == "":
        return ""
    try:
        value = Decimal(text)
    except InvalidOperation:
        return None
    if not value.is_finite():
        return None
    formatted = f"{value.quantize(Decimal('0.1')):.1f}"
    if formatted not in GRADES or value != Decimal(formatted):
        return None
    return formatted


def refuse(problems: list[Problem]):
    lines = [p.text for p in sorted(problems, key=lambda p: p.order)]
    raise invalid("Nothing was saved. Fix these and upload again:\n" + "\n".join(lines))


def _check(rows: list[tuple[int, str, str, str]], problems: list[Problem]) -> list[Entry]:
    seen: dict[str, str] = {}
    checked: list[Entry] = []
    for order, where, username, raw_grade in rows:
        username = username.strip()
        if username == "":
            problems.append(Problem(order, f"{where}: the username is empty"))
            continue
        grade = normalize_grade(raw_grade)
        if grade is None:
            problems.append(Problem(order, f"{where}: '{raw_grade.strip()}' is not a grade; use {_GRADE_LIST}"))
            continue
        if username in seen:
            problems.append(Problem(order, f"{where}: {username} already has a grade on {seen[username]}"))
            continue
        seen[username] = where
        checked.append(Entry(order, where, username, grade))
    return checked


def from_form(entries) -> tuple[list[Entry], list[Problem]]:
    if len(entries) > MAX_LINES:
        raise invalid(f"At most {MAX_LINES} grades can be sent at once.")
    problems: list[Problem] = []
    rows = [(i, e.username.strip() or f"row {i + 1}", e.username, e.grade) for i, e in enumerate(entries)]
    return _check(rows, problems), problems


def from_csv(data: bytes) -> tuple[list[Entry], list[Problem]]:
    """Lines of "username,grade". A first line naming the columns is skipped, and so are blank lines.
    A file that can't be read at all is refused at once; mistakes on lines are returned."""
    if len(data) > MAX_CSV_BYTES:
        raise invalid(f"The CSV file is {len(data):,} bytes; the limit is {MAX_CSV_BYTES:,}.")
    try:
        text = data.decode("utf-8-sig")   # -sig drops the byte order mark Excel writes
    except UnicodeDecodeError as exc:
        raise invalid(f"The CSV file is not UTF-8 text (byte {exc.start + 1} is not valid). "
                      "Save it as \"CSV UTF-8\" and upload it again.") from None

    rows: list[tuple[int, str, str, str]] = []
    problems: list[Problem] = []
    reader = csv.reader(io.StringIO(text, newline=""))
    first = True
    try:
        for cells in reader:
            line = reader.line_num
            where = f"line {line}"
            cells = [c.strip() for c in cells]
            if not any(cells):
                continue
            if first and cells[0].lower() in _HEADER_WORDS:
                first = False
                continue
            first = False
            if len(cells) < 2:
                problems.append(Problem(line, f"{where}: expected \"username,grade\" but found one column"))
                continue
            if any(cells[2:]):
                problems.append(Problem(line, f"{where}: expected \"username,grade\" but found {len(cells)} columns"))
                continue
            rows.append((line, where, cells[0], cells[1]))
            if len(rows) > MAX_LINES:
                raise invalid(f"The CSV file has more than {MAX_LINES} grade lines.")
    except csv.Error as exc:
        problems.append(Problem(reader.line_num, f"line {reader.line_num}: not valid CSV ({exc}); "
                                                 "the lines after it were not read"))

    if not rows and not problems:
        raise invalid("The CSV file has no grade lines. Each line should be \"username,grade\".")
    return _check(rows, problems), problems
