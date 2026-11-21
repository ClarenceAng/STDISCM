#!/bin/sh
# usage: sh run.sh     then put videos, or folders of videos, in the videos folder next to this file, and press Ctrl+C to stop
set -e

command -v g++ >/dev/null 2>&1 || { echo "g++ was not found on the PATH. It is needed to build the command line program." >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "docker was not found on the PATH." >&2; exit 1; }
command -v mkcert >/dev/null 2>&1 || {
    echo "mkcert was not found on the PATH. It makes the certificates for the HTTPS GUI and the producers' encrypted connections." >&2
    echo "Install it from https://github.com/FiloSottile/mkcert (for example: brew install mkcert, or apt install mkcert)." >&2
    exit 1
}

# the folder this script is in
root="$(cd "$(dirname "$0")" && pwd)"

# ---- certificates -------------------------------------------------------------------------------
# mkcert keeps one certificate authority per user (mkcert -CAROOT says where). certs/ gets a copy of
# its certificate, ca.pem, which the producers trust, and a certificate signed by it for the
# consumer. The authority's private key never leaves mkcert's folder.
certs="$root/certs"
mkdir -p "$certs/consumer" || { echo "Could not create the folder $certs/consumer for the certificates." >&2; exit 1; }
log="$certs/mkcert.log"

# Puts the authority into the system's and the browsers' trust stores, so they trust the GUI's
# certificate. The first time it may ask for a password; after that it does nothing. Java's store is
# left out: nothing here uses it, and changing it needs admin rights when JAVA_HOME is set.
if ! TRUST_STORES=system,nss mkcert -install >"$log" 2>&1; then
    cat "$log" >&2
    echo "mkcert could not add its certificate authority to the trust store, its message is above." >&2
    exit 1
fi

ca_folder="$(mkcert -CAROOT)" || { echo "mkcert did not say where its certificate authority is (mkcert -CAROOT)." >&2; exit 1; }
# An empty folder named ca.pem is what Docker leaves when the containers were started before the
# certificates existed (it mounts a missing file as a new folder). It would stop the copy below.
if [ -d "$certs/ca.pem" ]; then
    rmdir "$certs/ca.pem" || { echo "$certs/ca.pem is a folder with something in it, so mkcert's certificate authority can't be copied there." >&2; exit 1; }
fi
cp "$ca_folder/rootCA.pem" "$certs/ca.pem" ||
    { echo "Could not copy mkcert's certificate authority from $ca_folder/rootCA.pem to $certs/ca.pem." >&2; exit 1; }

# "consumer" is the name the producers connect to, localhost and 127.0.0.1 are what the browser uses.
# Made again on every start, so it always comes from the authority this computer trusts now.
if ! mkcert -cert-file "$certs/consumer/cert.pem" -key-file "$certs/consumer/key.pem" consumer localhost 127.0.0.1 ::1 >"$log" 2>&1; then
    cat "$log" >&2
    echo "mkcert could not make the consumer's certificate, its message is above." >&2
    exit 1
fi
rm -f "$log"

# ---- the command line program -------------------------------------------------------------------
g++ -std=c++17 -O2 -Wall -Wextra -pthread "$root/tools/upload_cli.cpp" -o "$root/tools/upload_cli"
"$root/tools/upload_cli" "$root/config.txt" "$root/docker-compose.yml"
