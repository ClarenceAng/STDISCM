#!/usr/bin/env bash
# Runs once, when the first primary creates the database. The standby gets these roles and the
# pg_hba.conf line by copying the primary's data directory.
set -euo pipefail

: "${APP_DB_PASSWORD:?APP_DB_PASSWORD must be set}"
: "${REPLICATION_PASSWORD:?REPLICATION_PASSWORD must be set}"

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" \
     -v app_pw="$APP_DB_PASSWORD" -v repl_pw="$REPLICATION_PASSWORD" <<'EOSQL'
-- the services connect as app; the standby streams the primary's WAL as replicator
CREATE ROLE app LOGIN PASSWORD :'app_pw';
CREATE ROLE replicator WITH REPLICATION LOGIN PASSWORD :'repl_pw';
EOSQL

# the image's pg_hba.conf allows normal connections from the network but not replication ones
echo "host replication replicator all scram-sha-256" >> "$PGDATA/pg_hba.conf"
