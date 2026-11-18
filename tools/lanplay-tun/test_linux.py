#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run in a disposable Linux container with a TUN device and NET_ADMIN."""
import socket
import struct
import subprocess
import sys
from pathlib import Path
from lanplay_tun import LDN_SCAN

CLIENT = "10.13.50.9"
GATEWAY = "10.13.254.254"
DISCOVERY_PORT = 11452
TUN = "rctest0"


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack("!" + "H" * (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return total ^ 0xffff


def datagram(src, dst, port, payload):
    ip = socket.inet_aton(src) + socket.inet_aton(dst)
    udp = struct.pack("!HHHH", DISCOVERY_PORT, port, 8 + len(payload), 0) + payload
    udp_sum = checksum(ip + struct.pack("!BBH", 0, 17, len(udp)) + udp) or 0xffff
    udp = udp[:6] + struct.pack("!H", udp_sum) + udp[8:]
    hdr = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0) + ip
    return hdr[:10] + struct.pack("!H", checksum(hdr)) + hdr[12:] + udp


def ip(*args):
    subprocess.run(["ip", *args], check=True)


with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
    relay.bind(("127.0.0.1", 0))
    relay.settimeout(3)
    helper = Path(__file__).with_name("lanplay_tun.py")
    proc = subprocess.Popen([sys.executable, str(helper), "--tun", TUN, "--client", CLIENT, "--wifi", "lo",
                             "--relay", f"127.0.0.1:{relay.getsockname()[1]}"])
    try:
        assert relay.recv(16) == b"\0"
        ip("link", "set", TUN, "up")
        ip("address", "add", CLIENT + "/32", "dev", "lo")
        ip("route", "add", "10.13.0.0/16", "dev", TUN, "src", CLIENT)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.bind((CLIENT, 0))
            client.settimeout(3)
            port = client.getsockname()[1]
            client.sendto(LDN_SCAN, ("127.0.0.1", DISCOVERY_PORT))
            frame, _ = relay.recvfrom(4096)
            assert frame[17:21] == socket.inet_aton(GATEWAY) and frame[29:] == LDN_SCAN
            client.sendto(b"discover", (GATEWAY, DISCOVERY_PORT))
            frame, bridge = relay.recvfrom(4096)
            assert frame[0] == 1 and frame[13:17] == socket.inet_aton(CLIENT)
            assert frame[17:21] == socket.inet_aton(GATEWAY) and frame[29:] == b"discover"

            # A different virtual client's traffic must never reach this TUN.
            relay.sendto(b"\1" + datagram(GATEWAY, "10.13.50.10", port, b"foreign"), bridge)
            relay.sendto(b"\1" + datagram(GATEWAY, "10.13.255.255", port, b"scan"), bridge)
            assert client.recvfrom(128) == (b"scan", (GATEWAY, DISCOVERY_PORT))
            relay.sendto(b"\1" + datagram("10.13.5.80", CLIENT, port, b"game"), bridge)
            assert client.recvfrom(128) == (b"game", ("10.13.5.80", DISCOVERY_PORT))
            client.sendto(b"game-reply", ("10.13.5.80", DISCOVERY_PORT))
            frame, _ = relay.recvfrom(4096)
            assert frame[17:21] == socket.inet_aton("10.13.5.80") and frame[29:] == b"game-reply"
            print("Linux TUN: client-to-relay discovery, unicast delivery, broadcast conversion and address isolation passed")
    finally:
        proc.terminate()
        proc.wait(timeout=3)
