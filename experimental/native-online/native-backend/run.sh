#!/bin/sh
set -eu
cd "$(dirname "$0")"
umask 077
mkdir -p .runtime .bin

# Own loopback CA and server certificate. Control-signing and TLS keys differ.
if [ ! -f .runtime/server.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 2 -keyout .runtime/ca-key.pem \
        -out .runtime/ca.pem -subj '/CN=RoomCircuit Native Lab CA' >.runtime/ca-build.log 2>&1
    openssl req -newkey rsa:2048 -nodes -keyout .runtime/server-key.pem \
        -out .runtime/server.csr -subj '/CN=127.0.0.1' >>.runtime/ca-build.log 2>&1
    printf '%s\n' 'subjectAltName = IP:127.0.0.1' 'extendedKeyUsage = serverAuth' >.runtime/tls.ext
    openssl x509 -req -in .runtime/server.csr -CA .runtime/ca.pem -CAkey .runtime/ca-key.pem -CAcreateserial \
        -days 2 -extfile .runtime/tls.ext -out .runtime/server.pem >>.runtime/ca-build.log 2>&1
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
        -out .runtime/issuer-key.pem >>.runtime/ca-build.log 2>&1
fi
test -f .runtime/issuer-key.pem
if [ ! -f .runtime/game-key ]; then
    openssl rand -hex 32 >.runtime/game-key
fi

account_env=${RC_ACCOUNT_LAB_ENV:-../account-lab/.runtime/lab.env}
export RC_DEVICE_ISSUER="${RC_DEVICE_ISSUER:-https://127.0.0.1:39280}"
export RC_ACCOUNT_URL="${RC_ACCOUNT_URL:-http://127.0.0.1:39170}"
export RC_LINKS_URL="${RC_LINKS_URL:-http://127.0.0.1:39171}"
export RC_LINKS_KEY="${RC_LINKS_KEY:-$(sed -n 's/^RC_IDENTITY_KEY=//p' "$account_env")}"
export RC_ACCOUNT_CORE_KEY="${RC_ACCOUNT_CORE_KEY:-$(sed -n 's/^RC_CORE_KEY=//p' "$account_env")}"
export RC_ACCOUNT_CORE_GRPC="${RC_ACCOUNT_CORE_GRPC:-127.0.0.1:39172}"
export RC_DEVICE_APPLICATIONS="${RC_DEVICE_APPLICATIONS:-01006fe013472000,01004ea00df70000,0000000000001234}"
export RC_DEVICE_GRANT_STATE="${RC_DEVICE_GRANT_STATE:-.runtime/approved-grants.json}"
export RC_DEVICE_GAME_KEY="${RC_DEVICE_GAME_KEY:-$(cat .runtime/game-key)}"
export RC_DEVICE_LISTEN="${RC_DEVICE_LISTEN:-127.0.0.1:39280}"
export RC_DEVICE_INTERNAL_LISTEN="${RC_DEVICE_INTERNAL_LISTEN:-127.0.0.1:39281}"
export RC_DEVICE_CERT="${RC_DEVICE_CERT:-.runtime/server.pem}"
export RC_DEVICE_TLS_KEY="${RC_DEVICE_TLS_KEY:-.runtime/server-key.pem}"
export RC_DEVICE_SIGNING_KEY="${RC_DEVICE_SIGNING_KEY:-.runtime/issuer-key.pem}"
if [ -n "${RC_DEVICE_BINARY:-}" ]; then
    exec "$RC_DEVICE_BINARY"
fi
go build -mod=readonly -trimpath -o .bin/native-backend .
exec .bin/native-backend
