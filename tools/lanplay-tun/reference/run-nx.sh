#!/bin/sh
set -eu
: "${RC_LANPLAY_PORT:?}"
client_ip=${RC_CLIENT_IP:-10.13.250.2}
printf '%s  %s\n' 2da236d4d57b63e233ea4b5e681f6474a1fb6634356a53c6aee7670ae710198e /upstream/dm/host/hle_ldn.c | sha256sum -c - >&2
# Three Darwin ABI details differ on Linux. Keep discovery, serialization,
# TCP handling, room membership and the title's LDN API calls from upstream.
sed -e '/^#include "hle.*\.h"/d' -e 's/\.sin_len = sizeof [a-z]*, //g' \
    -e '/setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE/d' \
    -e 's/pthread_setname_np("ldn-lan")/pthread_setname_np(pthread_self(), "ldn-lan")/' \
    /upstream/dm/host/hle_ldn.c >/tmp/test-hle-ldn.inc
cc -std=gnu11 -O2 -I/tmp /tools/reference/nx-driver.c -lpthread -o /tmp/rc-nx-reference
ip tuntap add dev rcgame0 mode tun
ip link set rcgame0 mtu 1400 up
ip address add "$client_ip/32" dev lo
ip route add 10.13.0.0/16 dev rcgame0 src "$client_ip"
python3 /tools/lanplay_tun.py --tun rcgame0 --client "$client_ip" --relay "host.docker.internal:$RC_LANPLAY_PORT" &
gateway=$!
trap 'kill "$gateway" || true' EXIT
sleep 0.2
/tmp/rc-nx-reference "$1" "$client_ip"
