-- The models of every node live in this one database, so a node only fails with the
-- database, not with another node. Each node reads and writes only the tables of its features.

CREATE EXTENSION IF NOT EXISTS pgcrypto;   -- crypt() hashes the seed passwords with bcrypt

CREATE TABLE users (
    id            bigserial PRIMARY KEY,
    username      text NOT NULL UNIQUE,
    password_hash text NOT NULL,           -- bcrypt
    full_name     text NOT NULL,
    role          text NOT NULL CHECK (role IN ('student', 'faculty', 'admin'))
);

-- One row per login. The token carries the id; every node checks the row on every request,
-- which is how a logout on the auth node reaches all the other nodes.
CREATE TABLE sessions (
    id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id    bigint NOT NULL REFERENCES users (id),
    issued_at  timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    revoked_at timestamptz
);

CREATE TABLE terms (
    code            text PRIMARY KEY,
    label           text NOT NULL,
    sort_order      integer NOT NULL UNIQUE,
    enrollment_open boolean NOT NULL DEFAULT false
);
-- at most one term open for enrollment
CREATE UNIQUE INDEX terms_one_enrollment_term ON terms (enrollment_open) WHERE enrollment_open;

CREATE TABLE courses (
    code  text PRIMARY KEY,
    title text NOT NULL,
    units integer NOT NULL CHECK (units BETWEEN 1 AND 6)
);

-- A section of a course in a term. "enrolled" is kept in step with the enrollments table by the
-- enrollment node, under a row lock on this row (SELECT ... FOR UPDATE).
CREATE TABLE offerings (
    id          bigserial PRIMARY KEY,
    course_code text NOT NULL REFERENCES courses (code),
    term_code   text NOT NULL REFERENCES terms (code),
    section     text NOT NULL,
    schedule    text NOT NULL DEFAULT '',
    room        text NOT NULL DEFAULT '',
    faculty_id  bigint REFERENCES users (id),
    capacity    integer NOT NULL CHECK (capacity BETWEEN 1 AND 500),
    enrolled    integer NOT NULL DEFAULT 0,
    is_open     boolean NOT NULL DEFAULT true,
    CONSTRAINT offerings_seats CHECK (enrolled BETWEEN 0 AND capacity),
    UNIQUE (course_code, term_code, section)
);
CREATE INDEX offerings_term ON offerings (term_code);
CREATE INDEX offerings_faculty ON offerings (faculty_id);

CREATE TABLE enrollments (
    id          bigserial PRIMARY KEY,
    offering_id bigint NOT NULL REFERENCES offerings (id),
    student_id  bigint NOT NULL REFERENCES users (id),
    status      text NOT NULL CHECK (status IN ('enrolled', 'dropped')),
    enrolled_at timestamptz NOT NULL DEFAULT now(),
    dropped_at  timestamptz,
    grade       numeric(2, 1) CHECK (grade IN (0.0, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0)),
    graded_at   timestamptz,
    graded_by   bigint REFERENCES users (id),
    UNIQUE (offering_id, student_id)
);
CREATE INDEX enrollments_student ON enrollments (student_id);

GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO app;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO app;
