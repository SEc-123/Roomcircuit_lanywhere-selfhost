#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if (( $# > 1 )) || [[ ${1:-} == --help ]]; then
    printf '%s\n' "Usage: bash $0 [computer's Wi-Fi IPv4]" \
        'With no address, emulator listeners accept local connections.'
    exit $(( $# > 1 ))
fi
node_bin=${ROOMCIRCUIT_NODE:-node}
"$node_bin" -e 'if (+process.versions.node.split(".")[0] < 22) { console.error("Node.js 22 or newer required"); process.exit(1); }'
test_bind=${1:-127.0.0.1}
"$node_bin" -e 'if (!require("node:net").isIPv4(process.argv[1])) process.exit(1)' "$test_bind"
if [[ ! -x server/eden/build/enet-host ]]; then
    sh server/eden/build.sh
fi
state=$(mktemp -d "${TMPDIR:-/tmp}/rc-crossplay.XXXXXXXX")
export LANYWHERE_STATE_DIR="$state" LANYWHERE_CAP_GB=2 LANYWHERE_PUBLIC_PORT=0 LANYWHERE_OPS_PORT=0 LANYWHERE_STATS_PORT=0
export LANYWHERE_ENABLE_PUBLIC_UPSTREAM=0 LANYWHERE_ENABLE_LANPLAY_UPSTREAM=0 LANYWHERE_ENABLE_COMMUNITY_DIRECT=0 LANYWHERE_SHUTDOWN_GRACE_MS=0
export LANYWHERE_UDP_BIND_ADDRESS="$test_bind" LANYWHERE_CONTROL_BIND_ADDRESS="$test_bind"
export ROOMCIRCUIT_EMULATOR_BIND="$test_bind" LANYWHERE_LANPLAY_BIND_ADDRESS="$test_bind" LANYWHERE_LANPLAY_PORT=11451
export ROOMCIRCUIT_EDEN_PORT=24872 ROOMCIRCUIT_RYULDN_PORT=30456
printf '%s\n' "Switch $test_bind:11470 (TCP+UDP)" "LAN Play $test_bind:11451 (UDP)" "Eden $test_bind:24872 (UDP)" "RyuLDN $test_bind:30456 (TCP)" "Test state $state"
exec "$node_bin" server/relay/server.js --port 11470
