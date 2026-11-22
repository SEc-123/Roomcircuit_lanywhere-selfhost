#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
project=${COMPOSE_PROJECT_NAME:-roomcircuit-native-account-lab}

compose() {
    docker compose -p "$project" --env-file .runtime/lab.env "$@"
}

case "${1:-up}" in
    up)
        test -x .bin/account && test -x .bin/identity
        mkdir -p .runtime
        if [ ! -f .runtime/lab.env ]; then
            if docker volume inspect "${project}_account-db" >/dev/null 2>&1; then
                echo 'Restore .runtime/lab.env for the existing account database before starting.' >&2
                exit 1
            fi
            python3 - <<'PY'
import os
from pathlib import Path
from secrets import token_hex
from tempfile import NamedTemporaryFile

path = Path('.runtime/lab.env')
with NamedTemporaryFile('w', dir=path.parent, delete=False) as stream:
    staging = Path(stream.name)
    for name in ('RC_DB_PASSWORD', 'RC_SESSION_SECRET', 'RC_CORE_KEY', 'RC_IDENTITY_KEY'):
        stream.write(f'{name}={token_hex(32)}\n')
    stream.flush()
    os.fsync(stream.fileno())
try:
    os.link(staging, path)
except FileExistsError:
    pass
finally:
    staging.unlink()
PY
        fi
        compose up -d
        ;;
    down)
        compose down
        ;;
    restart)
        compose restart account identity
        ;;
    *)
        echo 'Usage: sh lab.sh up|down|restart' >&2
        exit 2
        ;;
esac
