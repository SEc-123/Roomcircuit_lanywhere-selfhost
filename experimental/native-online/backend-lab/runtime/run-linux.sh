#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
umask 077
ulimit -c 0

if [ "$#" -ne 1 ]; then
    printf 'Usage: %s /path/to/owner-only-game-test.json\n' "$0" >&2
    exit 2
fi

package=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
export RC_NEX_RUNTIME_BINARY="$package/bin/roomcircuit-nexd"
export RC_NEX_RUNTIME_CREDENTIALS="$1"

exec "$package/bin/roomcircuit-nex-test" -test.v -test.count=1 -test.timeout=60s \
    '-test.run=^TestRoomCircuit(RuntimeConfiguration|RuntimeStartup|RuntimeProfiles|RuntimeTicketWindow|RuntimeSecureAdmission)$'
