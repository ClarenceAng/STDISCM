#!/usr/bin/env bash
# Starts one PostgreSQL node of the two-node cluster.
#
# The dbmonitor node writes the name of the current primary into $STATE_FILE (a shared volume).
#   - If it names this node, the node starts as the primary. The very first time, the official
#     entrypoint creates the database and runs the schema and seed scripts.
#   - If it names the peer, this node must be the peer's standby. A node that already is a standby
#     just starts and catches up. Any other data (empty, or an old primary replaced by a failover,
#     whose data may have diverged) is deleted and copied fresh from the peer with pg_basebackup.
# While PostgreSQL runs, the file is checked every second. If a failover named the peer while this
# node still runs as a primary (it was frozen or cut off and has come back), the node stops at once
# so it can't accept writes next to the new primary, and exits. Docker's restart policy then starts
# it again, and it comes back as the new primary's standby. This is called fencing.
# The same loop stops a primary from waiting for a standby that has gone (see stop_waiting_for_standby).

set -uo pipefail

: "${NODE_NAME:?NODE_NAME must be set, e.g. db1}"
: "${PEER_NAME:?PEER_NAME must be set, e.g. db2}"
: "${POSTGRES_PASSWORD:?POSTGRES_PASSWORD must be set}"
: "${REPLICATION_PASSWORD:?REPLICATION_PASSWORD must be set}"
STATE_FILE="${STATE_FILE:-/cluster-state/primary}"
DEFAULT_PRIMARY="${DEFAULT_PRIMARY:-db1}"
export PGDATA="${PGDATA:-/var/lib/postgresql/data}"

log() { echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) [$NODE_NAME] $*" >&2; }

# Prints the node the cluster state names as primary. Before the monitor has written the file,
# that is DEFAULT_PRIMARY.
designated_primary() {
    if [[ ! -e "$STATE_FILE" ]]; then
        echo "$DEFAULT_PRIMARY"
        return
    fi
    local name=""
    if ! read -r name < "$STATE_FILE"; then
        # read fails on a file with no newline at the end too, but still fills $name
        if [[ -z "$name" ]]; then
            log "could not read $STATE_FILE; assuming $DEFAULT_PRIMARY is the primary"
            echo "$DEFAULT_PRIMARY"
            return
        fi
    fi
    if [[ "$name" != "$NODE_NAME" && "$name" != "$PEER_NAME" ]]; then
        log "$STATE_FILE holds '$name', which is neither $NODE_NAME nor $PEER_NAME; assuming $DEFAULT_PRIMARY"
        echo "$DEFAULT_PRIMARY"
        return
    fi
    echo "$name"
}

# Exit status 0 when the peer accepts connections and runs as a primary (not in recovery).
# The reason it doesn't is left in $PEER_STATE for the log.
PEER_STATE=""
peer_is_primary() {
    local out
    if ! out=$(PGPASSWORD="$POSTGRES_PASSWORD" psql "host=$PEER_NAME user=postgres dbname=postgres connect_timeout=2" \
                 -XtAc "select pg_is_in_recovery()" 2>&1); then
        PEER_STATE="unreachable: $(echo "$out" | head -n 1)"
        return 1
    fi
    case "$out" in
        f) PEER_STATE="primary"; return 0 ;;
        t) PEER_STATE="standby"; return 1 ;;
        *) PEER_STATE="unexpected answer '$out'"; return 1 ;;
    esac
}

# Prints what the local server is: "standby", "primary", or "primary-waiting" when commits wait for
# a synchronous standby but none is streaming. Prints nothing while it doesn't answer (starting up).
local_state() {
    gosu postgres psql -h /var/run/postgresql -XtAc \
        "select case when pg_is_in_recovery() then 'standby'
                     when current_setting('synchronous_standby_names') <> ''
                          and not exists (select 1 from pg_stat_replication where state = 'streaming')
                     then 'primary-waiting'
                     else 'primary' end" 2>/dev/null
}

# A primary whose standby is gone would make every commit wait for ever. The monitor normally turns
# synchronous replication off first; this does it here too, so it also happens while the monitor
# is down. The monitor turns it back on once a standby streams again.
stop_waiting_for_standby() {
    log "no standby is streaming, so commits stop waiting for one"
    local out
    if ! out=$(gosu postgres psql -h /var/run/postgresql -Xq \
                 -c "ALTER SYSTEM SET synchronous_standby_names = ''" -c "SELECT pg_reload_conf()" 2>&1); then
        log "could not turn synchronous replication off: $out"
    fi
}

clone_from_peer() {
    log "deleting this node's data and copying the current primary $PEER_NAME with pg_basebackup"
    if ! find "$PGDATA" -mindepth 1 -delete; then
        log "could not empty $PGDATA"
        return 1
    fi
    mkdir -p "$PGDATA" && chown postgres:postgres "$PGDATA" && chmod 700 "$PGDATA" || {
        log "could not prepare $PGDATA for the copy"
        return 1
    }
    # -R writes standby.signal and primary_conninfo, so the server starts as $PEER_NAME's standby.
    # application_name is how the primary names this standby in pg_stat_replication.
    if ! gosu postgres pg_basebackup \
            -d "host=$PEER_NAME user=replicator password=$REPLICATION_PASSWORD application_name=$NODE_NAME connect_timeout=5" \
            -D "$PGDATA" -X stream -R -c fast; then
        log "pg_basebackup from $PEER_NAME failed (its message is above)"
        return 1
    fi
    log "copy from $PEER_NAME finished"
}

# ---- decide the role ------------------------------------------------------------------------

waiting_logged=""
while true; do
    primary="$(designated_primary)"
    if [[ "$primary" == "$NODE_NAME" ]]; then
        # Never start a second primary. This happens only if the monitor promoted the peer and then
        # stopped before recording it; the monitor adopts the running primary when it comes back.
        if peer_is_primary; then
            [[ -z "$waiting_logged" ]] && log "$PEER_NAME already runs as a primary although $STATE_FILE names $NODE_NAME; waiting for the monitor to record the real primary"
            waiting_logged=1
            sleep 3
            continue
        fi
        if [[ -f "$PGDATA/standby.signal" ]]; then
            log "$STATE_FILE names this node the primary but its data is a standby's; starting it as the primary"
            rm -f "$PGDATA/standby.signal" || { log "could not remove $PGDATA/standby.signal"; exit 1; }
        fi
        role=primary
        break
    fi

    if [[ -s "$PGDATA/PG_VERSION" && -f "$PGDATA/standby.signal" ]]; then
        role=standby   # already a standby; it reconnects to the primary by itself
        break
    fi
    if peer_is_primary; then
        clone_from_peer || exit 1   # the restart policy tries again
        role=standby
        break
    fi
    [[ "$waiting_logged" != "$PEER_STATE" ]] && log "waiting for $PEER_NAME, the primary, before copying its data ($PEER_STATE)"
    waiting_logged="$PEER_STATE"
    sleep 3
done
log "starting as the $role (the cluster state names $primary as primary)"

# ---- run PostgreSQL ---------------------------------------------------------------------------

# synchronous_commit=remote_apply: once the monitor sets synchronous_standby_names, a commit is
# only acknowledged after the standby has applied it, so promoting the standby loses nothing a
# user was told had succeeded, and reads served by the standby already show it.
# wal_sender_timeout: notice a frozen standby within 5 s instead of 60 s.
docker-entrypoint.sh postgres \
    -c listen_addresses='*' \
    -c cluster_name="$NODE_NAME" \
    -c wal_level=replica \
    -c hot_standby=on \
    -c wal_keep_size=512MB \
    -c wal_log_hints=on \
    -c wal_sender_timeout=5s \
    -c wal_receiver_timeout=5s \
    -c synchronous_commit=remote_apply &
pg_pid=$!

stop_postgres() {
    # fast shutdown (SIGINT): pooled client connections would make a smart shutdown wait
    kill -INT "$pg_pid" 2>/dev/null
    wait "$pg_pid"
    return $?
}
trap 'log "stop requested"; stop_postgres; exit $?' INT TERM

while kill -0 "$pg_pid" 2>/dev/null; do
    sleep 1 &
    wait $!
    state="$(local_state)"
    if [[ "$state" == primary* && "$(designated_primary)" == "$PEER_NAME" ]]; then
        log "fencing: $STATE_FILE now names $PEER_NAME as the primary, so this node stops accepting writes and restarts as its standby"
        stop_postgres
        exit 75
    fi
    if [[ "$state" == "primary-waiting" ]]; then
        stop_waiting_for_standby
    fi
done

wait "$pg_pid"
status=$?
log "postgres exited with status $status"
exit "$status"
