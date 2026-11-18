-- Demo data. Every account's password is "password".
--   admin                 the registrar (admin node)
--   msantos, jreyes,
--   acruz, mtan           faculty
--   student01..student60  students; 01-40 have grades from earlier terms

-- ---- accounts ------------------------------------------------------------------------------

INSERT INTO users (username, password_hash, full_name, role) VALUES
    ('admin',   crypt('password', gen_salt('bf', 8)), 'Registrar Admin', 'admin'),
    ('msantos', crypt('password', gen_salt('bf', 8)), 'Maria Santos',    'faculty'),
    ('jreyes',  crypt('password', gen_salt('bf', 8)), 'Jose Reyes',      'faculty'),
    ('acruz',   crypt('password', gen_salt('bf', 8)), 'Ana Cruz',        'faculty'),
    ('mtan',    crypt('password', gen_salt('bf', 8)), 'Miguel Tan',      'faculty');

INSERT INTO users (username, password_hash, full_name, role)
SELECT format('student%s', lpad(i::text, 2, '0')),
       crypt('password', gen_salt('bf', 8)),
       (ARRAY['Andrea', 'Benjamin', 'Carla', 'Daniel', 'Erika', 'Francis', 'Gabrielle', 'Hannah',
              'Ivan', 'Jasmine', 'Kevin', 'Lianne', 'Marco', 'Nicole', 'Oscar', 'Patricia',
              'Rafael', 'Sofia', 'Tristan', 'Vanessa'])[1 + (i - 1) % 20]
       || ' ' ||
       (ARRAY['Aquino', 'Bautista', 'Castillo', 'Dela Cruz', 'Estrada', 'Fernandez', 'Gonzales',
              'Hernandez', 'Ignacio', 'Jimenez', 'Lim', 'Mendoza', 'Navarro', 'Ocampo', 'Pascual',
              'Ramos', 'Salazar', 'Torres', 'Uy', 'Villanueva'])[1 + (i * 7) % 20],
       'student'
FROM generate_series(1, 60) AS i;

-- ---- terms and courses ----------------------------------------------------------------------

INSERT INTO terms (code, label, sort_order, enrollment_open) VALUES
    ('2025-26-T1', 'AY 2025-2026, Term 1', 1, false),
    ('2025-26-T2', 'AY 2025-2026, Term 2', 2, false),
    ('2025-26-T3', 'AY 2025-2026, Term 3', 3, false),
    ('2026-27-T1', 'AY 2026-2027, Term 1', 4, false),   -- running now, grades not posted yet
    ('2026-27-T2', 'AY 2026-2027, Term 2', 5, true);    -- open for enrollment

INSERT INTO courses (code, title, units) VALUES
    ('CCPROG1', 'Logic Formulation and Introductory Programming', 3),
    ('CCPROG2', 'Programming with Structured Data Types',         3),
    ('CCPROG3', 'Object-Oriented Programming',                    3),
    ('CCDSTRU', 'Discrete Structures',                            3),
    ('GEMATMW', 'Mathematics in the Modern World',                3),
    ('GEETHIC', 'Ethics',                                         3),
    ('CSARCH2', 'Computer Organization and Architecture',         3),
    ('CCINFOM', 'Information Management',                         3),
    ('CSALGCM', 'Algorithms and Complexity',                      3),
    ('CSNETWK', 'Computer Networks',                              3),
    ('CSOPESY', 'Operating Systems',                              3),
    ('STSWENG', 'Software Engineering',                           3),
    ('CSINTSY', 'Intelligent Systems',                            3),
    ('STDISCM', 'Distributed Computing',                          3),
    ('STADVDB', 'Advanced Database Systems',                      3),
    ('CSSECDV', 'Secure Software Development',                    3),
    ('CSMODEL', 'Modeling and Simulation',                        3),
    ('GEPCOMM', 'Purposive Communication',                        3);

-- ---- sections of earlier terms and of the running term: S11 and S12 of each course -----------

INSERT INTO offerings (course_code, term_code, section, schedule, room, faculty_id, capacity, is_open)
SELECT p.course_code, p.term_code, s.section,
       CASE s.section WHEN 'S11' THEN p.days_a ELSE p.days_b END || ' ' || p.hours,
       'Room ' || (300 + row_number() OVER (PARTITION BY p.term_code ORDER BY p.course_code, s.section)),
       (SELECT id FROM users WHERE username = p.faculty),
       40,
       false
FROM (VALUES
    ('2025-26-T1', 'CCPROG1', 'msantos', 'MW', 'TH', '07:30-09:00'),
    ('2025-26-T1', 'CCDSTRU', 'jreyes',  'MW', 'TH', '09:15-10:45'),
    ('2025-26-T1', 'GEMATMW', 'acruz',   'MW', 'TH', '11:00-12:30'),
    ('2025-26-T1', 'GEETHIC', 'mtan',    'MW', 'TH', '12:45-14:15'),
    ('2025-26-T2', 'CCPROG2', 'msantos', 'MW', 'TH', '07:30-09:00'),
    ('2025-26-T2', 'CSARCH2', 'acruz',   'MW', 'TH', '09:15-10:45'),
    ('2025-26-T2', 'CCINFOM', 'mtan',    'MW', 'TH', '11:00-12:30'),
    ('2025-26-T3', 'CCPROG3', 'msantos', 'MW', 'TH', '07:30-09:00'),
    ('2025-26-T3', 'CSALGCM', 'jreyes',  'MW', 'TH', '09:15-10:45'),
    ('2025-26-T3', 'CSNETWK', 'acruz',   'MW', 'TH', '11:00-12:30'),
    ('2026-27-T1', 'CSOPESY', 'msantos', 'MW', 'TH', '07:30-09:00'),
    ('2026-27-T1', 'STSWENG', 'mtan',    'MW', 'TH', '09:15-10:45'),
    ('2026-27-T1', 'CSINTSY', 'jreyes',  'MW', 'TH', '11:00-12:30')
) AS p (term_code, course_code, faculty, days_a, days_b, hours)
CROSS JOIN (VALUES ('S11'), ('S12')) AS s (section);

-- students 01-40 took every course of those terms: odd numbers in S11, even numbers in S12
INSERT INTO enrollments (offering_id, student_id, status, enrolled_at)
SELECT o.id, u.id, 'enrolled', now() - (6 - t.sort_order) * interval '4 months'
FROM users u
JOIN offerings o ON o.section = CASE WHEN substr(u.username, 8)::int % 2 = 1 THEN 'S11' ELSE 'S12' END
JOIN terms t ON t.code = o.term_code
WHERE u.role = 'student' AND substr(u.username, 8)::int <= 40 AND t.sort_order <= 4;

-- grades for the three finished terms, picked from the student and section ids so they vary
UPDATE enrollments e
SET grade = (ARRAY[4.0, 3.5, 3.5, 3.0, 3.0, 3.0, 2.5, 2.5, 2.0, 1.5, 1.0, 0.0])[1 + ((e.student_id * 31 + e.offering_id * 17) % 12)::int],
    graded_at = e.enrolled_at + interval '3 months',
    graded_by = o.faculty_id
FROM offerings o JOIN terms t ON t.code = o.term_code
WHERE o.id = e.offering_id AND t.sort_order <= 3;

-- ---- the term open for enrollment -------------------------------------------------------------

INSERT INTO offerings (course_code, term_code, section, schedule, room, faculty_id, capacity, is_open)
SELECT p.course_code, '2026-27-T2', p.section, p.schedule, p.room,
       (SELECT id FROM users WHERE username = p.faculty), p.capacity, p.is_open
FROM (VALUES
    ('STDISCM', 'S11', 'MW 07:30-09:00',  'Room 401', 'msantos', 40, true),
    ('STDISCM', 'S12', 'TH 07:30-09:00',  'Room 402', 'msantos',  3, true),   -- 2 of 3 seats taken below
    ('STADVDB', 'S11', 'MW 09:15-10:45',  'Room 403', 'acruz',   40, true),
    ('STADVDB', 'S12', 'TH 09:15-10:45',  'Room 404', 'acruz',   40, true),
    ('CSSECDV', 'S11', 'MW 11:00-12:30',  'Room 405', 'mtan',    30, false),  -- closed by the registrar
    ('CSMODEL', 'S11', 'TH 11:00-12:30',  'Room 406', 'jreyes',  35, true),
    ('GEPCOMM', 'S11', 'F 08:00-11:00',   'Room 407', 'acruz',   40, true)
) AS p (course_code, section, schedule, room, faculty, capacity, is_open);

INSERT INTO enrollments (offering_id, student_id, status)
SELECT o.id, u.id, 'enrolled'
FROM offerings o
JOIN users u ON u.username IN ('student02', 'student03')
WHERE o.term_code = '2026-27-T2' AND o.course_code = 'STDISCM' AND o.section = 'S12';

-- ---- seat counts ------------------------------------------------------------------------------

UPDATE offerings o
SET enrolled = (SELECT count(*) FROM enrollments e WHERE e.offering_id = o.id AND e.status = 'enrolled');
