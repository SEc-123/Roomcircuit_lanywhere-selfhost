#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Connect one Linux-routed game client to a RoomCircuit LAN Play endpoint."""

import argparse
import fcntl
import ipaddress
import os
import re
import selectors
import socket
import struct
import time

NETWORK = ipaddress.IPv4Network("10.13.0.0/16")
PREFIX = NETWORK.network_address.packed[:2]
BROADCAST = NETWORK.broadcast_address.packed
LIMITED_BROADCAST = b"\xff" * 4
GATEWAY = ipaddress.IPv4Address("10.13.254.254").packed
KEEPALIVE = b"\0"
IPV4 = b"\1"
TUNSETIFF, IFF_TUN, IFF_NO_PI = 0x400454CA, 1, 0x1000
MAX_WIRE_IP = 65507 - len(IPV4)
LDN_PORT = 11452
LDN_SCAN = bytes.fromhex("001445110000000000000000")


def update_checksum(checksum, old, new):
    """Replace the two address words in an existing one's-complement sum."""
    acc = checksum ^ 0xffff
    for offset in (0, 2):
        acc += int.from_bytes(old[offset:offset + 2], "big") ^ 0xffff
        acc += int.from_bytes(new[offset:offset + 2], "big")
    while acc >> 16:
        acc = (acc & 0xffff) + (acc >> 16)
    return acc ^ 0xffff


def ipv4_checksum(header):
    acc = sum(struct.unpack("!10H", header))
    while acc >> 16:
        acc = (acc & 0xffff) + (acc >> 16)
    return acc ^ 0xffff


def wifi_scan_frame(client, scan, port):
    # Accept only the fixed Local Wireless discovery frame from the provisioned device.
    if scan != LDN_SCAN or not 1 <= port <= 65535:
        return None
    udp = struct.pack("!HHHH", port, LDN_PORT, 8 + len(scan), 0) + scan
    header = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0) + client + GATEWAY
    return IPV4 + header[:10] + struct.pack("!H", ipv4_checksum(header)) + header[12:] + udp


def unpack(packet):
    if len(packet) < 20 or len(packet) > 65535 or packet[0] != 0x45:
        return None
    if int.from_bytes(packet[2:4], "big") != len(packet):
        return None
    if packet[9] not in (1, 6, 17):  # IPv4 echo, TCP and UDP only.
        return None
    return packet[12:16], packet[16:20]


def from_client(packet, client):
    if len(packet) > MAX_WIRE_IP:
        return None  # The routed interface uses an MTU of 1400.
    endpoints = unpack(packet)
    if not endpoints:
        return None
    source, dest = endpoints
    if source != client or dest == client or (dest[:2] != PREFIX and dest != LIMITED_BROADCAST):
        return None
    return IPV4 + packet


def from_relay(frame, client):
    if frame[:1] != IPV4:
        return None
    packet = frame[1:]
    endpoints = unpack(packet)
    if not endpoints:
        return None
    source, dest = endpoints
    if source[:2] != PREFIX or source in (client, BROADCAST, NETWORK.network_address.packed):
        return None
    if dest == client:
        return packet
    if dest not in (BROADCAST, LIMITED_BROADCAST) or packet[9] != 17:
        return None
    # Mobile apps restricted to unicast can receive discovery and game broadcasts this way.
    result = bytearray(packet)
    struct.pack_into("!H", result, 10, update_checksum(struct.unpack_from("!H", result, 10)[0], dest, client))
    result[16:20] = client
    if not (struct.unpack_from("!H", result, 6)[0] & 0x1fff):
        if len(result) < 28:
            return None
        udp_checksum = struct.unpack_from("!H", result, 26)[0]
        if udp_checksum:
            struct.pack_into("!H", result, 26, update_checksum(udp_checksum, dest, client) or 0xffff)
    return result


class RelayReceiver:
    """Reassemble switch-lan-play's type-3 transport fragments before TUN delivery."""

    def __init__(self, client):
        self.client = client
        self.pending = {}

    def receive(self, frame):
        if frame[:1] != b"\x03":
            return from_relay(frame, self.client)
        if len(frame) <= 17:
            return None
        source, dest = frame[1:5], frame[5:9]
        if source[:2] != PREFIX or dest not in (self.client, BROADCAST, LIMITED_BROADCAST):
            return None
        data = frame[17:]
        for order in ("<", ">"):
            ident, part, total, length, mtu = struct.unpack_from(order + "HBBHH", frame, 9)
            if length == len(data) and 0 < length <= mtu and part < total and part * mtu + length <= 65535 and (
                part == total - 1 or length == mtu
            ) and mtu * (total - 1) + 1 <= 65535:
                break
        else:
            return None
        now = time.monotonic()
        for key in list(self.pending):
            if self.pending[key][0] <= now:
                del self.pending[key]
        key = source, dest, ident
        record = self.pending.get(key)
        if record is None:
            if len(self.pending) >= 32:
                return None
            record = self.pending[key] = (now + 5, mtu, [None] * total)
        _, first_mtu, pieces = record
        if mtu != first_mtu or total != len(pieces):
            del self.pending[key]
            return None
        pieces[part] = data
        if any(piece is None for piece in pieces):
            return None
        del self.pending[key]
        packet = b"".join(pieces)
        if unpack(packet) != (source, dest):
            return None
        return from_relay(IPV4 + packet, self.client)


def attach_tun(name):
    fd = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH22x", name.encode(), IFF_TUN | IFF_NO_PI))
    except OSError:
        os.close(fd)
        raise
    return fd


def run(tun, relay, client, wifi=None):
    receiver = RelayReceiver(client)
    with selectors.DefaultSelector() as poller:
        poller.register(tun, selectors.EVENT_READ)
        poller.register(relay, selectors.EVENT_READ)
        if wifi:
            poller.register(wifi, selectors.EVENT_READ)
        keep = 0.0
        while True:
            now = time.monotonic()
            if now >= keep:
                try:
                    relay.send(KEEPALIVE)
                except (BlockingIOError, ConnectionRefusedError):
                    pass
                keep = now + 5
            for key, _ in poller.select(max(0, keep - time.monotonic())):
                try:
                    if key.fileobj == tun:
                        frame = from_client(os.read(tun, 65536), client)
                        if frame:
                            relay.send(frame)
                    elif key.fileobj == relay:
                        packet = receiver.receive(relay.recv(65536))
                        if packet:
                            os.write(tun, packet)
                    else:
                        message, (source, port) = wifi.recvfrom(64)
                        if socket.inet_aton(source) == client:
                            frame = wifi_scan_frame(client, message, port)
                            if frame:
                                relay.send(frame)
                except (BlockingIOError, ConnectionRefusedError):
                    continue


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tun", required=True, help="Existing Linux TUN interface (without packet-info headers)")
    parser.add_argument("--client", required=True, type=ipaddress.IPv4Address, help="Provisioned game client's virtual IPv4")
    parser.add_argument("--relay", required=True, help="RoomCircuit LAN Play host[:port]")
    parser.add_argument("--wifi", help="Linux LAN interface for a client's local UDP discovery broadcasts")
    args = parser.parse_args()
    if any(name and not re.fullmatch(r"[a-zA-Z0-9_.-]{1,15}", name) for name in (args.tun, args.wifi)):
        parser.error("Invalid Linux interface name")
    if args.client not in NETWORK or args.client.packed in (
        NETWORK.network_address.packed, BROADCAST, GATEWAY, b"\x0a\x0d\x25\x01",
    ):
        parser.error("Client must use an unreserved address within 10.13.0.0/16")
    host, sep, port = args.relay.partition(":")
    if not host or (sep and (not port.isdecimal() or not 1 <= int(port) <= 65535)):
        parser.error("Expected hostname or IPv4 followed by optional UDP port")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
        relay.connect((host, int(port) if sep else 11451))
        relay.setblocking(False)
        fd = attach_tun(args.tun)
        wifi = None
        try:
            if args.wifi:
                wifi = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                wifi.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, args.wifi.encode() + b"\0")
                wifi.setblocking(False)
                wifi.bind(("0.0.0.0", LDN_PORT))
            run(fd, relay, args.client.packed, wifi)
        except KeyboardInterrupt:
            pass
        finally:
            if wifi:
                wifi.close()
            os.close(fd)


if __name__ == "__main__":
    main()
