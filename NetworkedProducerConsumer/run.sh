#!/bin/sh
# usage: sh run.sh [video or folder] ...     any paths given are uploaded first, then it keeps listening for more
set -e

command -v g++ >/dev/null 2>&1 || { echo "g++ was not found on the PATH. It is needed to build the command line program." >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "docker was not found on the PATH." >&2; exit 1; }

# no cd, so relative paths still mean relative to where this was run from
root="$(cd "$(dirname "$0")" && pwd)"

g++ -std=c++17 -O2 -Wall -Wextra -pthread "$root/tools/upload_cli.cpp" -o "$root/tools/upload_cli"
"$root/tools/upload_cli" "$root/config.txt" "$root/docker-compose.yml" "$@"
