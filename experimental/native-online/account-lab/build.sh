#!/bin/sh
set -eu
cd "$(dirname "$0")"

account_revision=96b0593eea532d10d87b284146c4339a87f2d54c
if [ ! -d sources/account/.git ]; then
    mkdir -p sources
    GIT_TERMINAL_PROMPT=0 git -c credential.helper= clone https://github.com/openpak/account.git sources/account
    git -C sources/account checkout --detach "$account_revision"
fi
test "$(git -C sources/account rev-parse HEAD)" = "$account_revision"
test -z "$(git -C sources/account status --porcelain)"
mkdir -p .bin .cache/mod .cache/build
docker run --rm --platform linux/amd64 -v "$PWD:/lab" -w /lab \
    -e CGO_ENABLED=0 -e GOMODCACHE=/lab/.cache/mod -e GOCACHE=/lab/.cache/build \
    -e GOPROXY=https://proxy.golang.org -e GOTOOLCHAIN=local golang:1.26-alpine sh -ec '
        cd /lab/sources/account
        go build -mod=readonly -buildvcs=false -trimpath -o /lab/.bin/account ./cmd/account
        cd /lab
        go build -mod=readonly -trimpath -ldflags="-s -w" -o .bin/identity ./identity
    '
