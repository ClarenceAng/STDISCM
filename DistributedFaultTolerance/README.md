# Distributed Fault Tolerance (Online Enrollment System)

An online enrollment system split across nodes. Students sign in, see the courses offered, enroll in open sections and see their grades. Faculty upload grades. The registrar (admin) manages sections. Each feature runs on its own node, and the nodes talk to each other only over **gRPC**. When a node is down, only its own features stop working and the rest of the site keeps going.

Every node is a Docker container on one machine. The containers stand in for the separate machines in the specification.

The code is Python 3.12: Flask and Jinja for the web pages, grpcio between the nodes, and PostgreSQL 17 for the data.

## Nodes

| Node | Port | Serves | MVC part |
| --- | --- | --- | --- |
| `view` | 8080 (published) | the web pages | **View**: Jinja templates; the Flask routes only turn HTTP into gRPC calls |
| `auth` | 50051 | login, logout, signs session tokens | Controller + Model (users, sessions) |
| `course` | 50052 | terms and the sections offered in them | Controller + Model (terms, courses, offerings) |
| `enrollment` | 50053 | enroll, drop, my enrollments | Controller + Model (enrollments, seat counts) |
| `grade` | 50054 | students' grades; faculty class lists and grade upload | Controller + Model (grades) |
| `admin` | 50055 | create sections, change capacity, open/close, assign faculty (bonus) | Controller + Model |
| `dbmonitor` | 50056 | watches the two databases and fails over (bonus) | |
| `db1`, `db2` | 5432 (internal) | PostgreSQL primary and synchronous standby, either way round (bonus) | the data behind the Models |

Each backend node keeps its controller in `controller.py` (the gRPC methods) and its model in `model.py` (the SQL). The view node has no database access and no business rules.

## Files

```
docker-compose.yml          every node, its port, settings and volumes
run.bat, run.sh             build and start everything, then wait until the site and the databases are ready
proto/                      the gRPC contracts, one file per node
  common.proto                messages shared by several nodes (Term, User, Offering)
view/                       the view node
  app.py                      the pages: one route per page or form
  backend.py                  gRPC clients; turns every failure into a message naming the node and the feature
  templates/, static/         Jinja templates and the stylesheet
services/                   every backend node (one image; each node is started with its own command)
  common/                     shared by all backend nodes
    pool.py                     the connection pool (bonus)
    db.py                       transactions, reads that fall back to the standby, deadline propagation
    watchdog.py                 cuts off a database that stopped answering
    hosts.py                    looks the database nodes' addresses up in the background
    sessions.py                 checks the session token on every request
    rpc.py                      the wrapper every gRPC method goes through
    server.py                   starts a node: gRPC server, health service, pool statistics
    initialize.py               the one-off setup container: signing keys, cluster-state volume
  auth/ course/ enrollment/ grade/ admin/    controller.py, model.py, __main__.py each
  dbmonitor/monitor.py        failover
  tools/race_test.py          shows the row lock at work
database/                   the PostgreSQL nodes
  node-entrypoint.sh          starts a node as primary or standby, fences a replaced primary
  init/                       roles, schema and demo data, loaded the first time
```

## Building and running

You need **Docker** with Compose v2. On Windows, Docker Desktop must be running. Nothing else needs installing on the host, because everything is built inside the images. The first build downloads the base images and Python packages, which takes a few minutes.

From this folder, run

```bat
.\run.bat
```

or on Linux / macOS

```sh
sh run.sh
```

The script runs `docker compose up --build -d` and waits until the site answers and both databases are up and in sync (about 20 s after the build). Then open <http://localhost:8080>. To use another port, set `VIEW_PORT` first, for example `set VIEW_PORT=9090`.

The same by hand:

```sh
docker compose up --build -d      # build and start every node
docker compose ps                 # what is running
docker compose logs -f enrollment # one node's log
docker compose down               # stop and remove the containers; the data stays
docker compose down -v            # ... and delete the data, keys and cluster state too
```

### Accounts

Every password is `password`.

| Username | Role |
| --- | --- |
| `student01` … `student60` | students; 01–40 have grades from three earlier terms and classes in the running term |
| `msantos`, `jreyes`, `acruz`, `mtan` | faculty (`msantos` teaches CSOPESY this term, which has no grades yet) |
| `admin` | the registrar |

The term open for enrollment is AY 2026-2027 Term 2. STDISCM S12 has 3 seats with 2 taken, so one more student fills it. CSSECDV S11 is closed.

## Taking nodes down

Stop any node and use the site. The page that needs it says which node is missing and what that takes away, and every other page still works. The status page (link at the bottom of every page) shows each node's health, the database roles, replication and recent failover events, and the connection pools. It refreshes every 3 seconds.

```sh
docker compose stop course        # the node is down
docker compose start course       # back; its pages work again about 1-3 s later
docker compose pause course       # frozen instead of down: its pages time out after 8 s
docker compose unpause course
```

| Node down | What stops | What still works |
| --- | --- | --- |
| `view` | the web site | every other node keeps running. Sessions survive: after `start view`, everyone is still signed in |
| `auth` | signing in and out. Signing out still clears the browser, and says the session couldn't be ended on the server | everyone already signed in carries on, because every node checks tokens itself |
| `course` | the Courses page | enrolling (by course code and section on My enrollments), dropping, grades, admin |
| `enrollment` | enrolling, dropping, My enrollments | the course list, grades, admin |
| `grade` | My grades, My classes, grade upload | courses, enrolling, admin |
| `admin` | the Sections page | everything students and faculty do |
| `dbmonitor` | automatic failover, and the database part of the status page | everything else, including both databases |
| `db1` (the primary) | writes, for about 6–10 s until `db2` is promoted. Reads are served by `db2` meanwhile | everything, after the failover |
| `db2` (the standby) | nothing. A commit in progress waits up to about 2 s while the primary stops waiting for the standby | everything |
| both databases | everything that reads or writes data. Each page names the database as the cause, not its node | the site itself and the status page |

When a stopped database node is started again, it notices that the other node took over, copies the new primary's data and rejoins as its standby, in about 2–5 s.

## How fault tolerance is achieved

1. **One feature per node, and no node calls another.** Each backend node reads and writes the shared database itself. The course node doesn't ask the enrollment node anything, and the admin node lists sections without the course node. Losing one node can't take down a second one.
2. **Tokens every node checks by itself.** The auth node signs a JWT with its RSA private key (RS256), which only it holds. Every other node checks the signature with the public key, so it never needs the auth node. That is why signed-in users carry on while the auth node is down. The token names a row in the `sessions` table, which every node checks on every request, so signing out on the auth node ends the session on every node at once.
3. **A view that expects failure.** Every gRPC call has a deadline (8 s) and returns a result instead of raising. A failed call becomes a message naming the node and the feature it takes away, and the rest of the page is still drawn. Pages that need two calls make them at the same time, so a frozen node costs one deadline, not two.
4. **Finding nodes again.** When a container stops, Docker's DNS takes about 4 s to say its name is gone, and gRPC then waits up to 2 minutes before looking again. So the view keeps failing fast on the old channel while a background thread tries a new channel every second, and swaps it in when the node answers. A request that couldn't even connect (a recreated container has a new address) is retried once on a fresh channel, which is safe because the node never saw it.
5. **Deadline propagation.** A backend node gives its database work only the time the caller has left. If the caller has already given up, a change is rolled back instead of committed, so a user told "it failed" never finds it saved after all.
6. **Automatic restarts.** Every container has `restart: unless-stopped`, so a node that crashes is started again. `docker compose stop` counts as deliberate and is left alone.
7. **A redundant database with failover (bonus).** `db2` is a streaming-replication copy of `db1`. While it is attached, replication is synchronous (`synchronous_commit = remote_apply`): a commit is acknowledged only after the standby has applied it, so the standby has everything users were told succeeded. The `dbmonitor` node checks both once a second. When the primary misses 3 checks in a row while the standby answers, it promotes the standby with `pg_promote()` and records the new primary in a shared state file. It refuses to promote a standby that was not in sync, because that standby could be missing commits, and says so on the status page. When the standby goes away, the primary stops waiting for it: the monitor turns synchronous replication off, and so does the primary's own supervisor, so this works even while the monitor is down.
8. **Fencing.** A database node that comes back after being replaced (stopped, or frozen with `docker pause`) reads the state file, sees it is no longer the primary, and stops within a second. The restart policy then brings it back as a fresh standby of the new primary (`pg_basebackup`). There is never a second primary taking writes.
9. **Clients that follow the primary.** Each node's pool connects with `target_session_attrs=read-write` across both database nodes, so new connections find whichever one is the primary. The database nodes' addresses are looked up in the background, so a node whose name is gone is skipped without waiting for DNS. Every connection asks the server for its name, because Docker can hand a stopped node's address to the other node. A pooled connection that died with the old primary is caught by a 1 s `SELECT 1` check before any work, the pool's other idle connections to that node are closed, and the request is retried on a new connection.
10. **Reads survive the primary.** Reads fall back to the standby when the primary can't be reached, and for 3 s after it fails they go to the standby first. The page then says it was read from the standby. A session missing from a standby that is behind is reported as "can't be checked right now", not as signed out.
11. **A frozen node can't hang anything.** A frozen database server still acknowledges TCP, so neither TCP keepalives nor libpq notice. A watchdog shuts the socket down after 5 s (1 s for the `SELECT 1` check), which turns the hang into an error. `lock_timeout` (2 s) stops a request from queueing for ever behind a row lock, and `idle_in_transaction_session_timeout` (5 s) takes a row lock away from a node that froze while holding it.

## Row-level locking

Enrolling checks for a free seat and takes it as one step:

```sql
SELECT ... FROM offerings o JOIN terms t ON t.code = o.term_code
 WHERE o.id = %s
   FOR UPDATE OF o;                  -- lock this section's row until commit
-- checks: term open, section open, not enrolled already, a seat free
INSERT INTO enrollments ...;
UPDATE offerings SET enrolled = enrolled + 1 WHERE id = %s;
COMMIT;                              -- releases the lock
```

A second request for the same section waits at the `SELECT ... FOR UPDATE` until the first commits, then reads the seat count the first left behind. So two students can never both get the last seat. Requests for other sections lock other rows and run side by side. `OF o` locks only the section; without it the joined `terms` row would be locked as well, and every enrollment in the term would wait for every other one. Drop and the admin's capacity change lock the same row, and the grade upload takes `FOR SHARE` on it so the section can't be reassigned while grades are saved. Every transaction locks the section before any enrollment rows, so none can deadlock. The table also has `CHECK (enrolled BETWEEN 0 AND capacity)` as a last guard.

To see it, have 50 students press Enroll at the same instant for a new section with 5 seats:

```sh
docker compose run --rm race-test
docker compose run --rm race-test --seats 1 --students 60
```

It prints how many got in (exactly the number of seats) and how many were told the section is full. Afterwards the winners drop it again and the section is closed. The closed `LOCKTST` sections stay in the course list.

## Connection pooling (bonus)

`services/common/pool.py`. Each backend node has two pools, one to the primary (10 connections) and one to the standby (4). A request thread borrows a connection, runs its transaction and hands it back, so a request doesn't pay for a TCP connection, a password check and a new PostgreSQL process. A thread that finds all 10 lent out waits up to 2 s, and then gets "too busy" instead of hanging. Broken connections and ones idle for over 5 minutes are closed instead of reused. The status page shows every pool's counts; **Reused** is how many requests got a connection that was already open.

## When something goes wrong

Every failure is reported with its reason: on the page, in the node's log (`docker compose logs <node>`), or both.

| What happens | What the user sees |
| --- | --- |
| A node is stopped | "The Course node can't be reached, so viewing courses is unavailable right now. Everything else still works." with gRPC's own reason below it |
| A node is frozen | "... didn't answer within 8 s ...". For a change, it adds that the node rolls back what it couldn't finish |
| A node is up but neither database answers | "The Course node is running, but it can't use the database right now ...", with both databases' reasons |
| The primary is down and the page only reads | the page, with a note that it was read from the standby |
| The connection breaks while a change commits | "... the change may or may not have been saved. Reload the page to check." |
| Every pooled connection is busy for 2 s | "The enrollment node is too busy right now ... Try again." |
| A row lock is held for over 2 s | "Another request held a record this one needs ... Try again." |
| The session was signed out elsewhere, expired, or was signed with another key | the sign-in form, with the reason |
| Wrong username or password | one message for both, so usernames can't be probed. The auth log says which it was |
| Signing out while the auth node is down | signed out of this browser, with a note that the session itself ends at its expiry time |
| A full or closed section, a second section of the same course, dropping a graded course | the specific reason |
| A grade CSV with mistakes | every bad line with its line number, all at once, and nothing saved |
| The standby is behind and the primary is down | the monitor doesn't promote it and the status page says why; reads still work from the standby |
| The signing keys are missing or don't match | the setup container stops everything from starting and says how to fix it |
| A required setting is missing or out of range | the node refuses to start and names the setting |

## Limitations

- All containers run on one machine, so the machine itself is still a single point of failure. The addresses in `docker-compose.yml` can point at other machines, and the database nodes only need to reach each other and the monitor.
- The monitor is a single arbiter. While it is down nothing fails over, though everything else keeps working. Production systems run several monitors that agree by consensus (for example Patroni with etcd).
- After the primary turns synchronous replication off because the standby was gone, the monitor has to be running to turn it back on.
- A former primary rejoins by copying the whole database from the new primary, which is fine for this size of data.
- The passwords in `docker-compose.yml` are demo values.
