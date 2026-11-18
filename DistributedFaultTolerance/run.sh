#!/bin/sh
# Builds every image, starts every node in the background and waits until the site answers.
set -e
cd "$(dirname "$0")"

command -v docker >/dev/null 2>&1 || { echo "docker was not found on the PATH." >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "Docker is installed but not running. Start it and try again." >&2; exit 1; }
docker compose version >/dev/null 2>&1 || { echo "\"docker compose\" is not available. Docker Compose v2 is needed." >&2; exit 1; }

docker compose up --build -d || { echo "Starting the containers failed. The messages above say why." >&2; exit 1; }

port="${VIEW_PORT:-8080}"
echo "Waiting for the site on http://localhost:$port ..."
i=0
until curl -s -o /dev/null "http://localhost:$port/login"; do
    i=$((i + 1))
    if [ "$i" -ge 60 ]; then
        echo "The site did not answer within 2 minutes. See: docker compose logs view" >&2
        exit 1
    fi
    sleep 2
done

# The first start also creates and copies the database, which takes a little longer.
echo "Waiting for the database nodes ..."
i=0
until curl -s "http://localhost:$port/status" | grep -q "Commits wait for the standby"; do
    i=$((i + 1))
    if [ "$i" -ge 90 ]; then
        echo "The database nodes were not ready within 3 minutes. The status page says why:" >&2
        echo "  http://localhost:$port/status   or run: docker compose logs db1 db2 dbmonitor" >&2
        exit 1
    fi
    sleep 2
done

cat <<EOF

Ready: http://localhost:$port   (all passwords are "password")
  student01..student60, msantos / jreyes / acruz / mtan (faculty), admin

Stop a node:       docker compose stop course
Start it again:    docker compose start course
Stop everything:   docker compose down      (add -v to also delete the database)
EOF
