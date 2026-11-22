#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
mkdir -p .runtime .bin
if [ ! -f .runtime/issuer-key.pem ]; then
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out .runtime/issuer-key.pem >.runtime/key-build.log 2>&1
fi
if [ ! -f .runtime/na-issuer-key.pem ]; then
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out .runtime/na-issuer-key.pem >.runtime/na-key-build.log 2>&1
fi
if [ -n "${NX_LAB_DEVICE_HOST:-}" ] && [ -z "${NX_LAB_DAUTH_SIGNING_KEY:-}" ]; then
    export NX_LAB_DAUTH_SIGNING_KEY=.runtime/dauth-issuer-key.pem
    if [ ! -f "$NX_LAB_DAUTH_SIGNING_KEY" ]; then
        dauth_key_tmp=$(mktemp "$NX_LAB_DAUTH_SIGNING_KEY.XXXXXX")
        trap 'unlink "$dauth_key_tmp" 2>/dev/null || :' EXIT
        openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$dauth_key_tmp" >.runtime/dauth-key-build.log 2>&1
        if ! ln "$dauth_key_tmp" "$NX_LAB_DAUTH_SIGNING_KEY" && [ ! -f "$NX_LAB_DAUTH_SIGNING_KEY" ]; then
            exit 1
        fi
        unlink "$dauth_key_tmp"
        trap - EXIT
    fi
fi
if [ ! -f .runtime/game-key ]; then
    openssl rand -hex 32 >.runtime/game-key
fi
export NX_LAB_ISSUER="${NX_LAB_ISSUER:-https://127.0.0.1:39282}"
export NX_LAB_LISTEN="${NX_LAB_LISTEN:-127.0.0.1:39282}"
export NX_LAB_INTERNAL="${NX_LAB_INTERNAL:-127.0.0.1:39283}"
export NX_LAB_GRANT_URL="${NX_LAB_GRANT_URL:-http://127.0.0.1:39281}"
export NX_LAB_GRANT_KEY="${NX_LAB_GRANT_KEY:-$(cat ../.runtime/game-key)}"
export NX_LAB_GAME_KEY="${NX_LAB_GAME_KEY:-$(cat .runtime/game-key)}"
export NX_LAB_APPLICATION="${NX_LAB_APPLICATION:-0000000000001234}"
export NX_LAB_SIGNING_KEY="${NX_LAB_SIGNING_KEY:-.runtime/issuer-key.pem}"
export NX_LAB_CERT="${NX_LAB_CERT:-../.runtime/server.pem}"
export NX_LAB_TLS_KEY="${NX_LAB_TLS_KEY:-../.runtime/server-key.pem}"
export NX_LAB_STATE="${NX_LAB_STATE:-.runtime/devices.enc}"
export NX_LAB_STATE_KEY="${NX_LAB_STATE_KEY:-.runtime/device-state-key}"
if [ -n "${NX_LAB_DEVICE_HOST:-}" ]; then
    export NX_LAB_OAUTH_REDIRECT="${NX_LAB_OAUTH_REDIRECT:-nintendo://account.nx.sys}"
    export NX_LAB_OAUTH_CLIENT_ID="${NX_LAB_OAUTH_CLIENT_ID:-6ffd70c434d303c8}"
else
    export NX_LAB_OAUTH_REDIRECT="${NX_LAB_OAUTH_REDIRECT:-roomcircuit://device-link}"
    export NX_LAB_OAUTH_CLIENT_ID="${NX_LAB_OAUTH_CLIENT_ID:-roomcircuit-owned-client}"
fi
export NX_LAB_NA_ISSUER="${NX_LAB_NA_ISSUER:-$NX_LAB_ISSUER}"
export NX_LAB_NA_SIGNING_KEY="${NX_LAB_NA_SIGNING_KEY:-.runtime/na-issuer-key.pem}"
export NX_LAB_BROWSER_CA="${NX_LAB_BROWSER_CA:-../.runtime/ca.pem}"
export NX_LAB_BROWSER_ORIGIN="${NX_LAB_BROWSER_ORIGIN:-https://127.0.0.1:39280}"
export NX_LAB_ACCOUNT_URL="${NX_LAB_ACCOUNT_URL:-http://127.0.0.1:39170}"
if [ ! -f "$NX_LAB_STATE_KEY" ]; then
    if [ -e "$NX_LAB_STATE" ]; then
        echo 'Restore the device state encryption key before starting.' >&2
        exit 1
    fi
    state_key_tmp=$(mktemp "$NX_LAB_STATE_KEY.XXXXXX")
    trap 'unlink "$state_key_tmp" 2>/dev/null || :' EXIT
    openssl rand -hex 32 >"$state_key_tmp"
    if ! ln "$state_key_tmp" "$NX_LAB_STATE_KEY" && [ ! -f "$NX_LAB_STATE_KEY" ]; then
        exit 1
    fi
    unlink "$state_key_tmp"
    trap - EXIT
fi
if [ -n "${NX_LAB_BINARY:-}" ]; then
    exec "$NX_LAB_BINARY"
fi
go build -mod=readonly -trimpath -o .bin/nx-adapter .
exec .bin/nx-adapter
