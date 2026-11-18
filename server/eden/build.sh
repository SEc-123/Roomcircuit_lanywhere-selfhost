#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

src="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
out="${1:-$src/build}"
mkdir -p "$out"
out="$(CDPATH='' cd -- "$out" && pwd)"
version=1.3.18
digest=28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
tmp="$(mktemp -d "${TMPDIR:-/tmp}/roomcircuit-enet.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

archive="$tmp/enet.tar.gz"
if [ -n "${ROOMCIRCUIT_ENET_ARCHIVE:-}" ]; then
    cp "$ROOMCIRCUIT_ENET_ARCHIVE" "$archive"
else
    curl --fail --location --silent --show-error --max-time 60         "https://codeload.github.com/lsalzman/enet/tar.gz/refs/tags/v$version" -o "$archive"
fi
if command -v sha256sum >/dev/null 2>&1; then
    printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
else
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
fi
tar -xzf "$archive" -C "$tmp"
enet="$tmp/enet-$version"

"${CC:-cc}" -O2 -std=gnu11     -DHAS_FCNTL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1     -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1     -I "$enet/include" -c "$src/enet-host.c" -o "$tmp/host.o"
set -- "$enet"/callbacks.c "$enet"/compress.c "$enet"/host.c "$enet"/list.c     "$enet"/packet.c "$enet"/peer.c "$enet"/protocol.c "$enet"/unix.c
"${CC:-cc}" -O2 -std=gnu11     -DHAS_FCNTL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1     -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1     -I "$enet/include" "$tmp/host.o" "$@" -o "$tmp/enet-host"
install -m 0755 "$tmp/enet-host" "$out/enet-host"
install -m 0644 "$enet/LICENSE" "$out/ENet-LICENSE"
if [ "${ROOMCIRCUIT_ENET_BUILD_TEST:-0}" = 1 ]; then
    "${CC:-cc}" -O2 -std=gnu11         -DHAS_FCNTL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1         -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1         -DENET_TEST_CLIENT -I "$enet/include" "$src/enet-host.c" "$@" -o "$out/enet-test-client"
fi
