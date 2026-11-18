#!/bin/sh
set -eu
ip tuntap add dev rcgame0 mode tun
ip link set rcgame0 mtu 1400 up
ip address add "$RC_CLIENT_IP/32" dev lo
ip route add 10.13.0.0/16 dev rcgame0 src "$RC_CLIENT_IP"
python3 /tools/lanplay_tun.py --tun rcgame0 --client "$RC_CLIENT_IP" --relay "host.docker.internal:$RC_LANPLAY_PORT" &
gateway=$!
trap 'kill "$gateway" || true' EXIT
sleep 0.15
dotnet /provider/Melo.dll "$1" "$RC_CLIENT_IP" "$2"
