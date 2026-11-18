"""The view node: the V of MVC, on a node of its own.

It renders the pages and owns nothing else: no database and no rules about enrolling or grading.
Each page asks the node that owns its feature over gRPC. Those nodes hold the controllers and
models. If a node is down, the page that needs it says so and every other page keeps working.
"""

import hashlib
import logging
import os
import secrets
import sys
import time
from datetime import datetime
from functools import wraps
from urllib.parse import urlparse
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import jwt
from cryptography.hazmat.primitives import serialization
from flask import Flask, abort, flash, g, redirect, render_template, request, url_for
from werkzeug.exceptions import HTTPException, RequestEntityTooLarge

import admin_pb2
import auth_pb2
import common_pb2
import course_pb2
import dbcluster_pb2
import enrollment_pb2
import grade_pb2
from backend import NODES, Backend, Failure, Result

logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                    format="%(asctime)s %(levelname)-7s [view] %(name)s: %(message)s")
log = logging.getLogger("view")

COOKIE = "token"
GRADES = ("4.0", "3.5", "3.0", "2.5", "2.0", "1.5", "1.0", "0.0")


# ---- configuration ----------------------------------------------------------------------------

def _startup_failure(message: str):
    log.error("cannot start: %s", message)
    sys.exit(2)


def _setting(name: str, default: str | None = None) -> str:
    value = os.environ.get(name, "").strip() or default
    if value is None:
        _startup_failure(f"environment variable {name} is not set")
    return value


try:
    RPC_TIMEOUT = float(_setting("RPC_TIMEOUT", "8"))
except ValueError:
    _startup_failure("RPC_TIMEOUT is not a number of seconds")
try:
    DISPLAY_TZ = ZoneInfo(_setting("DISPLAY_TZ", "Asia/Manila"))
except (ZoneInfoNotFoundError, ValueError):
    _startup_failure(f"DISPLAY_TZ={os.environ.get('DISPLAY_TZ')!r} is not a time zone name")

_key_path = _setting("PUBLIC_KEY_PATH")
try:
    with open(_key_path, "rb") as _f:
        _public_key = serialization.load_pem_public_key(_f.read())
except FileNotFoundError:
    _startup_failure(f"{_key_path} does not exist (did the setup container run?)")
except OSError as _exc:
    _startup_failure(f"could not read {_key_path}: {_exc.strerror}")
except ValueError as _exc:
    _startup_failure(f"{_key_path} is not a PEM public key ({_exc})")

backend = Backend({name: _setting(spec.env) for name, spec in NODES.items()}, RPC_TIMEOUT)


# ---- reading the session token ------------------------------------------------------------------

_KID = hashlib.sha256(_public_key.public_bytes(
    serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)).hexdigest()[:16]


def read_token(token: str) -> tuple[dict | None, str | None]:
    """Who the cookie says is signed in, checked with the auth node's public key, so the view can
    show the right menu without asking the auth node. Whether the session was signed out is
    checked by the node that serves each request, against the database."""
    try:
        if jwt.get_unverified_header(token).get("kid") != _KID:
            return None, "Your session cookie wasn't signed with this system's current key. Sign in again."
        claims = jwt.decode(token, _public_key, algorithms=["RS256"], audience="enrollment-system",
                            issuer="auth-node", options={"require": ["exp", "sub", "sid", "role"]})
        return {"id": int(claims["sub"]), "username": claims.get("username", ""), "name": claims.get("name", ""),
                "role": claims["role"], "exp": claims["exp"]}, None
    except jwt.ExpiredSignatureError:
        return None, "Your session has expired. Sign in again."
    except (jwt.InvalidTokenError, ValueError) as exc:
        return None, f"Your session cookie is not valid ({exc}). Sign in again."


# ---- Flask ------------------------------------------------------------------------------------

app = Flask(__name__)
# Signs only the one-shot flash messages; the session itself is the JWT cookie. A random key per
# start is enough because the view runs as one process (see the Dockerfile), and a restart only
# loses a message that hadn't been shown yet.
app.secret_key = secrets.token_bytes(32)
app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024
app.config["SESSION_COOKIE_SAMESITE"] = "Lax"


class SignedOut(Exception):
    """A node refused the session (signed out elsewhere, expired, keys changed)."""

    def __init__(self, failure: Failure):
        super().__init__(failure.message)
        self.failure = failure


def check(result: Result) -> Result:
    if result.failure is not None and result.failure.kind == "signed_out":
        raise SignedOut(result.failure)
    return result


def first_failure(*results: Result) -> Failure | None:
    return next((r.failure for r in results if r.failure is not None), None)


def from_standby(*results: Result) -> bool:
    return any(r.from_standby for r in results)


def flash_failure(failure: Failure) -> None:
    flash({"text": failure.message, "detail": failure.detail}, "error")


def flash_ok(text: str) -> None:
    flash({"text": text}, "success")


def safe_next(target: str | None) -> str | None:
    """Only a path on this site, so a crafted link can't send someone elsewhere after signing in."""
    if target and target.startswith("/") and not target.startswith("//"):
        return target
    return None


def page(*roles: str):
    """The page needs a signed-in user, with one of `roles` when given."""

    def decorate(view):
        @wraps(view)
        def wrapper(*args, **kwargs):
            if g.user is None:
                return redirect(url_for("login_form", next=request.full_path if request.method == "GET" else None))
            if roles and g.user["role"] not in roles:
                plural = {"student": "students", "faculty": "faculty", "admin": "admins"}
                return render_template("error.html", code=403, title="Not for this account",
                                       message=f"This page is for {' and '.join(plural[r] for r in roles)} only."), 403
            return view(*args, **kwargs)
        return wrapper

    return decorate


@app.before_request
def before_request():
    # Forms only from this site's own pages. The cookie is also SameSite=Lax, which already keeps
    # other sites' forms from carrying it.
    if request.method == "POST":
        origin = request.headers.get("Origin")
        if origin and urlparse(origin).netloc != request.host:
            log.warning("refused a POST to %s sent from %s", request.path, origin)
            abort(403, description="This form was sent from another site, so it was refused.")

    g.user = g.token = None
    g.drop_cookie = False
    token = request.cookies.get(COOKIE)
    if token:
        user, problem = read_token(token)
        if user is not None:
            g.user, g.token = user, token
        else:
            g.drop_cookie = True
            flash({"text": problem}, "warning")


@app.after_request
def after_request(response):
    if g.get("drop_cookie"):
        response.delete_cookie(COOKIE)
    return response


NAV = {
    "student": [("courses", "Courses"), ("enrollments", "My enrollments"), ("grades", "My grades")],
    "faculty": [("classes", "My classes"), ("courses", "Courses")],
    "admin": [("admin_sections", "Sections"), ("courses", "Courses")],
}


@app.context_processor
def template_globals():
    links = [{"url": url_for(endpoint), "label": label, "active": request.endpoint == endpoint}
             for endpoint, label in NAV.get(g.user["role"], [])] if g.get("user") else []
    return {"user": g.get("user"), "nav": links, "GRADES": GRADES}


@app.template_filter("when")
def when(unix_seconds) -> str:
    if not unix_seconds:
        return ""
    return datetime.fromtimestamp(int(unix_seconds), DISPLAY_TZ).strftime("%b %d, %Y %I:%M:%S %p")


@app.template_filter("seat_state")
def seat_state(offering) -> str:
    if not offering.open:
        return "closed"
    if offering.enrolled >= offering.capacity:
        return "full"
    return "open"


# ---- signing in and out (auth node) -----------------------------------------------------------

@app.get("/")
def home():
    if g.user is None:
        return redirect(url_for("login_form"))
    return redirect(url_for({"student": "courses", "faculty": "classes", "admin": "admin_sections"}[g.user["role"]]))


@app.get("/login")
def login_form():
    if g.user is not None:
        return redirect(url_for("home"))
    return render_template("login.html", auth=backend.health("auth", timeout=1.0),
                           next=safe_next(request.args.get("next")))


@app.post("/login")
def login():
    username = request.form.get("username", "")
    nxt = safe_next(request.form.get("next"))
    result = backend.call("auth", "Login", auth_pb2.LoginRequest(username=username,
                                                                 password=request.form.get("password", "")))
    if not result.ok:
        status = {"down": 503, "timeout": 504, "database": 503, "busy": 503,
                  "signed_out": 401, "refused": 400}.get(result.failure.kind, 500)
        return render_template("login.html", failure=result.failure, username=username, next=nxt), status

    reply = result.reply
    response = redirect(nxt or url_for("home"))
    response.set_cookie(COOKIE, reply.token, max_age=max(0, reply.expires_at - int(time.time())),
                        httponly=True, samesite="Lax")
    flash_ok(f"Signed in as {reply.user.full_name}.")
    return response


@app.post("/logout")
def logout():
    response = redirect(url_for("login_form"))
    response.delete_cookie(COOKIE)
    if g.token is None:
        return response
    result = backend.call("auth", "Logout", auth_pb2.LogoutRequest(), token=g.token)
    if result.ok:
        flash_ok("Signed out.")
    else:
        flash({"text": f"Signed out of this browser only. {result.failure.message} "
                       f"The session itself ends on its own at {when(g.user['exp'])}.",
               "detail": result.failure.detail}, "warning")
    return response


# ---- courses (course node) and enrolling (enrollment node) -------------------------------------

@app.get("/courses")
@page()
def courses():
    term = request.args.get("term", "").strip()
    terms, listing = backend.in_parallel(
        (backend.call, "course", "ListTerms", common_pb2.ListTermsRequest(), g.token),
        (backend.call, "course", "ListOfferings", course_pb2.ListOfferingsRequest(term_code=term), g.token),
    )
    check(terms)
    check(listing)
    return render_template("courses.html", terms=terms.reply.terms if terms.ok else [],
                           listing=listing.reply if listing.ok else None,
                           failure=first_failure(listing, terms), from_standby=from_standby(terms, listing))


def _offering_id_from_form() -> int | None:
    try:
        return int(request.form.get("offering_id", ""))
    except ValueError:
        return None


@app.post("/enroll")
@page("student")
def enroll():
    back = safe_next(request.form.get("back")) or url_for("courses")
    if "offering_id" in request.form:            # the Enroll button on the course list
        offering_id = _offering_id_from_form()
        if offering_id is None:
            flash({"text": "The form didn't say which section to enroll in."}, "error")
            return redirect(back)
        req = enrollment_pb2.EnrollRequest(offering_id=offering_id)
    else:                                         # the form on My enrollments, which needs no course node
        req = enrollment_pb2.EnrollRequest(course_code=request.form.get("course_code", ""),
                                           section=request.form.get("section", ""))
    result = check(backend.call("enrollment", "Enroll", req, g.token))
    if result.ok:
        o = result.reply.offering
        flash_ok(f"Enrolled in {o.course_code} {o.section} ({o.title}). {o.enrolled} of {o.capacity} seats are taken now.")
    else:
        flash_failure(result.failure)
    return redirect(back)


@app.post("/drop")
@page("student")
def drop():
    back = safe_next(request.form.get("back")) or url_for("enrollments")
    offering_id = _offering_id_from_form()
    if offering_id is None:
        flash({"text": "The form didn't say which section to drop."}, "error")
        return redirect(back)
    result = check(backend.call("enrollment", "Drop", enrollment_pb2.DropRequest(offering_id=offering_id), g.token))
    if result.ok:
        o = result.reply.offering
        flash_ok(f"Dropped {o.course_code} {o.section} ({o.title}).")
    else:
        flash_failure(result.failure)
    return redirect(back)


@app.get("/enrollments")
@page("student")
def enrollments():
    result = check(backend.call("enrollment", "ListMyEnrollments", enrollment_pb2.ListMyEnrollmentsRequest(), g.token))
    return render_template("enrollments.html", data=result.reply, failure=result.failure,
                           from_standby=result.from_standby)


# ---- grades (grade node) ------------------------------------------------------------------------

@app.get("/grades")
@page("student")
def grades():
    result = check(backend.call("grade", "ListMyGrades", grade_pb2.ListMyGradesRequest(), g.token))
    return render_template("grades.html", data=result.reply, failure=result.failure, from_standby=result.from_standby)


@app.get("/classes")
@page("faculty")
def classes():
    result = check(backend.call("grade", "ListMyClasses", grade_pb2.ListMyClassesRequest(), g.token))
    return render_template("classes.html", data=result.reply, failure=result.failure, from_standby=result.from_standby)


@app.get("/classes/<int:offering_id>")
@page("faculty")
def roster(offering_id: int):
    result = check(backend.call("grade", "GetClassRoster", grade_pb2.GetClassRosterRequest(offering_id=offering_id), g.token))
    return render_template("roster.html", offering_id=offering_id, data=result.reply, failure=result.failure,
                           from_standby=result.from_standby)


def _report_upload(result: Result) -> None:
    if result.ok:
        r = result.reply
        flash_ok(f"Grades saved: {r.changed} changed, {r.unchanged} left as they were.")
    else:
        flash_failure(result.failure)


@app.post("/classes/<int:offering_id>/grades")
@page("faculty")
def upload_form_grades(offering_id: int):
    entries = [grade_pb2.GradeEntry(username=key.split(":", 1)[1], grade=value)
               for key, value in request.form.items() if key.startswith("grade:")]
    if not entries:
        flash({"text": "There were no grades in the form."}, "error")
    else:
        _report_upload(check(backend.call("grade", "UploadGrades", grade_pb2.UploadGradesRequest(
            offering_id=offering_id, entries=entries), g.token)))
    return redirect(url_for("roster", offering_id=offering_id))


@app.post("/classes/<int:offering_id>/csv")
@page("faculty")
def upload_csv_grades(offering_id: int):
    file = request.files.get("file")
    if file is None or file.filename == "":
        flash({"text": "Choose a CSV file to upload first."}, "error")
        return redirect(url_for("roster", offering_id=offering_id))
    data = file.read()
    if not data:
        flash({"text": f"{file.filename} is empty."}, "error")
        return redirect(url_for("roster", offering_id=offering_id))
    _report_upload(check(backend.call("grade", "UploadGrades", grade_pb2.UploadGradesRequest(
        offering_id=offering_id, csv=data), g.token)))
    return redirect(url_for("roster", offering_id=offering_id))


# ---- sections (admin node) ------------------------------------------------------------------------

@app.get("/admin")
@page("admin")
def admin_sections():
    term = request.args.get("term", "").strip()
    terms, faculty, listing = backend.in_parallel(
        (backend.call, "admin", "ListTerms", common_pb2.ListTermsRequest(), g.token),
        (backend.call, "admin", "ListFaculty", admin_pb2.ListFacultyRequest(), g.token),
        (backend.call, "admin", "ListOfferings", admin_pb2.AdminListOfferingsRequest(term_code=term), g.token),
    )
    for r in (terms, faculty, listing):
        check(r)
    return render_template("admin.html", terms=terms.reply.terms if terms.ok else [],
                           faculty=faculty.reply.faculty if faculty.ok else [],
                           listing=listing.reply if listing.ok else None,
                           failure=first_failure(listing, terms, faculty),
                           from_standby=from_standby(terms, faculty, listing))


def _whole_number(field: str, label: str, blank: int | None = None) -> int:
    raw = request.form.get(field, "").strip()
    if raw == "" and blank is not None:
        return blank
    try:
        return int(raw)
    except ValueError:
        raise ValueError(f"{label} must be a whole number.") from None


@app.post("/admin/sections")
@page("admin")
def create_section():
    term_code = request.form.get("term_code", "").strip()
    back = url_for("admin_sections", term=term_code or None)
    try:
        req = admin_pb2.CreateOfferingRequest(
            course_code=request.form.get("course_code", ""),
            title=request.form.get("title", ""),
            units=_whole_number("units", "Units", blank=0),
            term_code=term_code,
            section=request.form.get("section", ""),
            schedule=request.form.get("schedule", ""),
            room=request.form.get("room", ""),
            faculty_id=_whole_number("faculty_id", "Faculty", blank=0),
            capacity=_whole_number("capacity", "Capacity"),
            open=request.form.get("open") == "on",
        )
    except ValueError as exc:
        flash({"text": str(exc)}, "error")
        return redirect(back)
    result = check(backend.call("admin", "CreateOffering", req, g.token))
    if result.ok:
        o = result.reply
        flash_ok(f"Added {o.course_code} {o.section} to {o.term.label} with {o.capacity} seats.")
    else:
        flash_failure(result.failure)
    return redirect(back)


@app.post("/admin/sections/<int:offering_id>")
@page("admin")
def update_section(offering_id: int):
    back = url_for("admin_sections", term=request.form.get("term_code") or None)
    try:
        req = admin_pb2.UpdateOfferingRequest(
            offering_id=offering_id,
            capacity=_whole_number("capacity", "Capacity"),
            open=request.form.get("open") == "on",
            faculty_id=_whole_number("faculty_id", "Faculty", blank=0),
        )
    except ValueError as exc:
        flash({"text": str(exc)}, "error")
        return redirect(back)
    result = check(backend.call("admin", "UpdateOffering", req, g.token))
    if result.ok:
        o = result.reply
        flash_ok(f"Saved {o.course_code} {o.section}: {o.capacity} seats, {'open' if o.open else 'closed'}, "
                 f"{o.faculty_name or 'no faculty'}.")
    else:
        flash_failure(result.failure)
    return redirect(back)


# ---- system status (every node) -------------------------------------------------------------------

@app.get("/status")
def status():
    names = list(NODES)
    pooled = [n for n in names if n != "dbmonitor"]
    results = backend.in_parallel(
        *[(backend.health, n) for n in names],
        *[(backend.info, n) for n in pooled],
        (backend.call, "dbmonitor", "GetStatus", dbcluster_pb2.GetStatusRequest(), None, 2.0),
    )
    healths = results[:len(names)]
    infos = dict(zip(pooled, results[len(names):len(names) + len(pooled)]))
    cluster = results[-1]
    return render_template("status.html", healths=healths, infos=infos, cluster=cluster, now=time.time())


# ---- errors -------------------------------------------------------------------------------------

@app.errorhandler(SignedOut)
def on_signed_out(exc: SignedOut):
    # The sign-in form is shown right here rather than redirected to: /login sends anyone whose
    # cookie still looks valid back to their home page, which would loop if the browser kept the
    # cookie. The cookie is dropped all the same.
    g.drop_cookie = True
    g.user = None
    return render_template("login.html", signed_out=exc.failure,
                           next=safe_next(request.full_path) if request.method == "GET" else None), 401


@app.errorhandler(RequestEntityTooLarge)
def on_too_large(exc):
    return render_template("error.html", code=413, title="File too large",
                           message="Uploads can be at most 2 MB."), 413


@app.errorhandler(HTTPException)
def on_http_error(exc: HTTPException):
    return render_template("error.html", code=exc.code, title=exc.name, message=exc.description), exc.code


@app.errorhandler(Exception)
def on_unexpected(exc: Exception):
    log.exception("unexpected error on %s %s", request.method, request.path)
    return render_template("error.html", code=500, title="Something went wrong in the view node",
                           message=f"{type(exc).__name__}: {exc}"), 500
