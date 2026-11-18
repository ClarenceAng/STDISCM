"""Shows the row lock on a section at work.

Creates a new section with a few seats (through the admin node), signs many students in (auth
node), and has all of them press Enroll at the same instant (enrollment node). Exactly as many
students as there are seats must get in, and the seat count must match the enrollments.

    docker compose run --rm race-test                  # 5 seats, 50 students
    docker compose run --rm race-test --seats 3 --students 60
"""

import argparse
import sys
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

import grpc

import admin_pb2
import admin_pb2_grpc
import auth_pb2
import auth_pb2_grpc
import common_pb2
import enrollment_pb2
import enrollment_pb2_grpc
from common import config

TIMEOUT = 20


def bearer(token: str):
    return (("authorization", f"Bearer {token}"),)


def fail(message: str) -> None:
    print(f"race test failed: {message}", file=sys.stderr)
    sys.exit(1)


def call(what: str, fn, *args, **kwargs):
    try:
        return fn(*args, timeout=TIMEOUT, **kwargs)
    except grpc.RpcError as exc:
        fail(f"{what}: {exc.code().name}: {exc.details()}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Many students enroll in a few seats at the same instant.")
    parser.add_argument("--seats", type=int, default=5)
    parser.add_argument("--students", type=int, default=50)
    args = parser.parse_args()
    if not 1 <= args.seats <= 500:
        fail("--seats must be from 1 to 500")
    if not 1 <= args.students <= 60:
        fail("--students must be from 1 to 60 (the demo data has student01..student60)")

    auth = auth_pb2_grpc.AuthServiceStub(grpc.insecure_channel(config.text("AUTH_ADDR")))
    admin = admin_pb2_grpc.AdminServiceStub(grpc.insecure_channel(config.text("ADMIN_ADDR")))
    enrollment = enrollment_pb2_grpc.EnrollmentServiceStub(grpc.insecure_channel(config.text("ENROLLMENT_ADDR")))

    admin_token = call("signing in as admin", auth.Login, auth_pb2.LoginRequest(username="admin", password="password")).token
    terms = call("listing terms", admin.ListTerms, common_pb2.ListTermsRequest(), metadata=bearer(admin_token)).terms
    term = next((t for t in terms if t.enrollment_open), None)
    if term is None:
        fail("no term is open for enrollment")

    section = f"R{int(time.time()) % 1000:03d}"
    offering = call("creating the test section", admin.CreateOffering, admin_pb2.CreateOfferingRequest(
        course_code="LOCKTST", title="Row Lock Test", units=1, term_code=term.code, section=section,
        schedule="race test", room="-", capacity=args.seats, open=True), metadata=bearer(admin_token))
    print(f"created LOCKTST {section} in {term.label} with {args.seats} seats (section id {offering.id})")

    usernames = [f"student{i:02d}" for i in range(1, args.students + 1)]
    with ThreadPoolExecutor(max_workers=16) as pool:
        tokens = list(pool.map(lambda u: call(f"signing in {u}", auth.Login,
                                              auth_pb2.LoginRequest(username=u, password="password")).token, usernames))
    print(f"signed in {len(tokens)} students; all of them press Enroll at once")

    start = threading.Barrier(len(tokens))
    results: list[str] = [""] * len(tokens)

    def press(i: int) -> None:
        start.wait()   # every thread is released at the same moment
        try:
            enrollment.Enroll(enrollment_pb2.EnrollRequest(offering_id=offering.id),
                              metadata=bearer(tokens[i]), timeout=TIMEOUT)
            results[i] = "enrolled"
        except grpc.RpcError as exc:
            results[i] = f"{exc.code().name}: {exc.details()}"

    started = time.monotonic()
    threads = [threading.Thread(target=press, args=(i,)) for i in range(len(tokens))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    took = time.monotonic() - started

    for outcome, count in Counter(results).most_common():
        print(f"  {count:3d} x {outcome}")

    final = call("reading the section back", admin.ListOfferings,
                 admin_pb2.AdminListOfferingsRequest(term_code=term.code), metadata=bearer(admin_token))
    seats_taken = next(o.enrolled for o in final.offerings if o.id == offering.id)
    winners = results.count("enrolled")
    print(f"{winners} enrolled, seat count {seats_taken}/{args.seats}, in {took:.2f} s")

    # leave the students as they were: the winners drop the test section, which is then closed
    for i, outcome in enumerate(results):
        if outcome == "enrolled":
            call("dropping the test section", enrollment.Drop,
                 enrollment_pb2.DropRequest(offering_id=offering.id), metadata=bearer(tokens[i]))
    call("closing the test section", admin.UpdateOffering, admin_pb2.UpdateOfferingRequest(
        offering_id=offering.id, capacity=args.seats, open=False), metadata=bearer(admin_token))
    for token in tokens + [admin_token]:
        call("signing out", auth.Logout, auth_pb2.LogoutRequest(), metadata=bearer(token))

    expected = min(args.seats, args.students)
    if winners != expected or seats_taken != expected:
        fail(f"expected exactly {expected} enrollments and a seat count of {expected}")
    print(f"PASS: exactly {expected} of {args.students} students got the {args.seats} seats")


if __name__ == "__main__":
    main()
