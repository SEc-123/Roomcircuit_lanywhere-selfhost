#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rc-reference-eden.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT
revision=d85bad89248fcae68f812d7e1c937436261404cb
manifest="$here/upstream.sha256"
wire_udp=4
case "${1:-}" in
    '') ;;
    --yuzu)
        revision=74d0865478679c38a3bd24b5f02a3b2d3be662f7
        manifest="$here/yuzu.sha256"
        wire_udp=3
        ;;
    *) echo 'Usage: verify.sh [--yuzu]' >&2; exit 2 ;;
esac
echo "Building upstream RoomMember at $revision"
mkdir -p "$tmp/source" "$tmp/shim/common"
while read -r hash file; do
    mkdir -p "$tmp/source/$(dirname -- "$file")"
    if [[ -n "${EDEN_SOURCE:-}" ]]; then
        cp "$EDEN_SOURCE/src/$file" "$tmp/source/$file"
    else
        curl -fsSL --max-time 30 "https://git.eden-emu.dev/eden-emu/eden/raw/commit/$revision/src/$file" -o "$tmp/source/$file"
    fi
done < "$manifest"
verify() {
    if command -v sha256sum >/dev/null; then sha256sum -c "$1"; else shasum -a 256 -c "$1"; fi
}
(cd "$tmp/source" && verify "$manifest")
if [[ -n "${ROOMCIRCUIT_ENET_ARCHIVE:-}" ]]; then
    cp "$ROOMCIRCUIT_ENET_ARCHIVE" "$tmp/enet.tar.gz"
else
    curl -fsSL --max-time 60 https://codeload.github.com/lsalzman/enet/tar.gz/refs/tags/v1.3.18 -o "$tmp/enet.tar.gz"
fi
printf '%s  %s\n' 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd enet.tar.gz > "$tmp/enet.sha256"
(cd "$tmp" && verify enet.sha256 && tar -xzf enet.tar.gz)
enet="$tmp/enet-1.3.18"
objects=()
for file in callbacks compress host list packet peer protocol unix; do
    "${CC:-cc}" -std=gnu11 -O2 -DHAS_FCNTL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 \
        -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -I"$enet/include" -c "$enet/$file.c" -o "$tmp/$file.o"
    objects+=("$tmp/$file.o")
done
# Remove only the dependency on Eden's process-wide assertion/logging backend.
# Assert conditions are still evaluated. No networking functions are stubbed.
mkdir -p "$tmp/shim/common/logging"
printf '#pragma once\n' >"$tmp/shim/common/logging/log.h"
cat >"$tmp/shim/common/assert.h" <<'H'
#pragma once
#include <cassert>
#define ASSERT_MSG(condition, ...) assert(condition)
H
"${CXX:-c++}" -std=c++20 -pthread -O2 "-DREFERENCE_UDP=$wire_udp" -include algorithm -include cstring \
    -I"$tmp/shim" -I"$tmp/source" -I"$enet/include" "$here/probe.cpp" \
    "$tmp/source/network/room_member.cpp" "$tmp/source/network/packet.cpp" "${objects[@]}" -o "$tmp/reference-room-member"
if [[ ! -x "$here/../build/enet-host" ]]; then "$here/../build.sh"; fi
node "$here/run.cjs" "$tmp/reference-room-member" "$@"
