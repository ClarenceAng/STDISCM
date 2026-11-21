#!/bin/sh
# Makes the TLS certificates, builds every image, starts every node in the background and waits until the site answers.
set -e
cd "$(dirname "$0")"

command -v docker >/dev/null 2>&1 || { echo "docker was not found on the PATH." >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "Docker is installed but not running. Start it and try again." >&2; exit 1; }
docker compose version >/dev/null 2>&1 || { echo "\"docker compose\" is not available. Docker Compose v2 is needed." >&2; exit 1; }
command -v mkcert >/dev/null 2>&1 || {
    echo "mkcert was not found on the PATH. It makes the certificates for HTTPS and for the encrypted gRPC links between nodes." >&2
    echo "Install it from https://github.com/FiloSottile/mkcert (for example: brew install mkcert, or apt install mkcert)." >&2
    exit 1
}

# ---- certificates -------------------------------------------------------------------------------
# mkcert keeps one certificate authority per user (mkcert -CAROOT says where). certs/ gets a copy of
# its certificate, ca.pem, which the view trusts, and one certificate signed by it for each node, for
# the name the view dials. The authority's private key never leaves mkcert's folder.
certs="$(pwd)/certs"
mkdir -p "$certs" || { echo "Could not create the folder $certs for the certificates." >&2; exit 1; }
log="$certs/mkcert.log"

# Puts the authority into the system's and the browsers' trust stores, so they trust the site's
# certificate. The first time it may ask for a password; after that it does nothing. Java's store is
# left out: nothing here uses it, and changing it needs admin rights when JAVA_HOME is set.
if ! TRUST_STORES=system,nss mkcert -install >"$log" 2>&1; then
    cat "$log" >&2
    echo "mkcert could not add its certificate authority to the trust store, its message is above." >&2
    exit 1
fi

ca_folder="$(mkcert -CAROOT)" || { echo "mkcert did not say where its certificate authority is (mkcert -CAROOT)." >&2; exit 1; }

# Running nodes only read their certificates when they start. Ones from the same authority are still
# good, but if the authority changed, every node has to start again to load the new ones.
recreate=""
if [ -e "$certs/ca.pem" ] && ! cmp -s "$ca_folder/rootCA.pem" "$certs/ca.pem"; then
    recreate="--force-recreate"
fi
# An empty folder named ca.pem is what Docker leaves when the containers were started before the
# certificates existed (it mounts a missing file as a new folder). It would stop the copy below.
if [ -d "$certs/ca.pem" ]; then
    rmdir "$certs/ca.pem" || { echo "$certs/ca.pem is a folder with something in it, so mkcert's certificate authority can't be copied there." >&2; exit 1; }
fi
cp "$ca_folder/rootCA.pem" "$certs/ca.pem" ||
    { echo "Could not copy mkcert's certificate authority from $ca_folder/rootCA.pem to $certs/ca.pem." >&2; exit 1; }

# make_cert <folder> <names...>; made again on every start, so they always come from the
# authority this computer trusts now
make_cert() {
    folder="$1"
    shift
    mkdir -p "$certs/$folder" || { echo "Could not create the folder $certs/$folder." >&2; exit 1; }
    if ! mkcert -cert-file "$certs/$folder/cert.pem" -key-file "$certs/$folder/key.pem" "$@" >"$log" 2>&1; then
        cat "$log" >&2
        echo "mkcert could not make the certificate for the $folder node, its message is above." >&2
        exit 1
    fi
    # mkcert makes the key readable by this user only, and the nodes run as user 10001 in their
    # containers. These keys are for this computer's own containers and nothing else.
    chmod 644 "$certs/$folder/key.pem" || { echo "Could not let the containers read $certs/$folder/key.pem." >&2; exit 1; }
}

# each backend node's certificate is for its compose service name only
for node in auth course enrollment grade admin dbmonitor; do
    make_cert "$node" "$node"
done
# the view's is for what the browser opens
make_cert view localhost 127.0.0.1 ::1 view
rm -f "$log"

# ---- containers ---------------------------------------------------------------------------------
if [ -n "$recreate" ]; then
    echo "certs/ca.pem was not mkcert's current certificate authority, so every node is started again to load the new certificates."
fi
docker compose up --build -d $recreate || { echo "Starting the containers failed. The messages above say why." >&2; exit 1; }

# curl checks the site's certificate against mkcert's authority, the same way a browser does.
# --ssl-revoke-best-effort: curl built on Windows' own TLS also asks whether the certificate was
# revoked, and mkcert's certificates name nowhere to ask, so that check would always fail.
port="${VIEW_PORT:-8080}"
site="https://localhost:$port"
echo "Waiting for the site on $site ..."
i=0
until curl -s --cacert "$certs/ca.pem" --ssl-revoke-best-effort -o /dev/null "$site/login"; do
    status=$?
    if [ "$status" -eq 60 ]; then
        # the site answered but its certificate was refused, which waiting won't fix
        echo "The site answered, but its certificate was refused. curl's reason:" >&2
        curl -sS --cacert "$certs/ca.pem" --ssl-revoke-best-effort -o /dev/null "$site/login" || true
        echo "Run run.sh again to make new certificates. If this repeats, see: docker compose logs view" >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 60 ]; then
        echo "The site did not answer within 2 minutes. curl's last reason:" >&2
        curl -sS --cacert "$certs/ca.pem" --ssl-revoke-best-effort -o /dev/null "$site/login" || true
        echo "See: docker compose logs view" >&2
        exit 1
    fi
    sleep 2
done

# The first start also creates and copies the database, which takes a little longer.
echo "Waiting for the database nodes ..."
i=0
until curl -s --cacert "$certs/ca.pem" --ssl-revoke-best-effort "$site/status" | grep -q "Commits wait for the standby"; do
    i=$((i + 1))
    if [ "$i" -ge 90 ]; then
        echo "The database nodes were not ready within 3 minutes. The status page says why:" >&2
        echo "  $site/status   or run: docker compose logs db1 db2 dbmonitor" >&2
        exit 1
    fi
    sleep 2
done

cat <<EOF

Ready: $site   (all passwords are "password")
  student01..student60, msantos / jreyes / acruz / mtan (faculty), admin

Stop a node:       docker compose stop course
Start it again:    docker compose start course
Stop everything:   docker compose down      (add -v to also delete the database)
EOF
