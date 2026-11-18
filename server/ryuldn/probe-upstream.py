#!/usr/bin/env python3
"""Check RyuLDN framing against Ryubing's pinned reference healthcheck codecs."""
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import types

UPSTREAM_SHA = 'ba32bcf0878184b3f30b3db4e2d3888a08aa17dc'
REPO = Path(__file__).resolve().parents[2]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('upstream', type=Path, help='clean clone of Ryubing/LdnServer at the pinned commit')
args = parser.parse_args()
checkout = args.upstream.resolve()
sha = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
status = subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain', '--untracked-files=no'], text=True)
if sha != UPSTREAM_SHA or status:
    parser.error(f'expected an unmodified Ryubing/LdnServer checkout at {UPSTREAM_SHA}')

# Load only the wire codec. The full healthcheck client uses newer Python syntax
# and depends on unrelated packages. This probe works on Python 3.9 or later.
healthcheck_dir = checkout / 'py-healthcheck'
sys.path.insert(0, str(healthcheck_dir))
package = types.ModuleType('ldn_healthcheck.ryuldn')
package.__path__ = [str(healthcheck_dir / 'ldn_healthcheck' / 'ryuldn')]
sys.modules[package.__name__] = package

from ldn_healthcheck.ryuldn.header import PacketHeader
from ldn_healthcheck.ryuldn.packets.initialize import InitializePacket
from ldn_healthcheck.ryuldn.packets.create_access_point import CreateAccessPointPacket
with socket.socket() as free:
    free.bind(('127.0.0.1', 0))
    port = free.getsockname()[1]

def read(source):
    raw = source.read(12)
    header = PacketHeader.from_buffer(raw)
    assert header.is_valid()
    return header.packet_id.value, source.read(header.data_size)

def frame(kind, body):
    return struct.pack('<IBB2xi', 0x4e444c52, kind, 1, len(body)) + body

def joined(peer, source):
    kind, config = read(source)
    assert kind == 17
    ip, mask = struct.unpack('<II', config)
    assert ip & mask == 0x0a0d0000 and mask == 0xffff0000
    kind, network = read(source)
    assert kind == 15 and len(network) == 0x480
    return ip, network

def init(peer, source):
    packet = InitializePacket()
    peer.sendall(packet.encode())
    kind, body = read(source)
    assert kind == 0
    packet.decode(frame(kind, body))
    assert any(packet.client_id) and len(packet.mac_address) == 6
    return body

with tempfile.TemporaryDirectory() as d:
    env = dict(os.environ, ROOMCIRCUIT_RYULDN_PORT=str(port), LANYWHERE_STATS_PORT='0',
               LANYWHERE_PUBLIC_PORT='0', LANYWHERE_STATE_DIR=d, LANYWHERE_SHUTDOWN_GRACE_MS='0')
    server = subprocess.Popen(['node', 'server/relay/server.js', '--port', '0'], env=env, cwd=REPO,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 7
        while True:
            try:
                first = socket.create_connection(('127.0.0.1', port), 0.25)
                break
            except ConnectionRefusedError:
                if server.poll() is not None or time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        with first, first.makefile('rb') as f, socket.create_connection(('127.0.0.1',port), 1) as second, second.makefile('rb') as s:
            first.settimeout(3)
            second.settimeout(3)
            identity = init(first, f)
            init(second, s)
            host = CreateAccessPointPacket()
            host.username = b'Upstream Wire Probe'
            host.local_communication_id = 0x0100152000022000
            host.node_count_max = 4
            host.local_communication_version = 42
            first.sendall(host.encode())
            ip1, network = joined(first, f)
            assert struct.unpack_from('<Q', network)[0] == host.local_communication_id
            assert struct.unpack_from('<H', network, 0x68+46)[0] == 42
            scan = bytearray(96)
            struct.pack_into('<Q', scan, 0, host.local_communication_id)
            scan[0x5c] = 1
            missing = bytearray(scan)
            struct.pack_into('<Q', missing, 0, host.local_communication_id + 1)
            second.sendall(frame(10, missing))
            assert read(s) == (12, b'')
            second.sendall(frame(10, scan))
            assert read(s) == (11, network)
            assert read(s) == (12, b'')
            connect = bytearray(0x4fc)
            connect[0x44:0x4a] = b'Second'
            connect[0x7c:] = network
            struct.pack_into('<I', connect, 0x74, 43)
            second.sendall(frame(13, connect))
            assert read(s) == (255, struct.pack('<i', 4))
            struct.pack_into('<I', connect, 0x74, 42)
            second.sendall(frame(13, connect))
            ip2, network = joined(second, s)
            assert network[0x67] == 2
            assert read(f) == (7, network)
            data = b'first-to-second'
            payload = struct.pack('<IHIHII', ip1, 9500, ip2, 9501, 17, len(data)) + data
            forged = bytearray(payload)
            struct.pack_into('<I', forged, 0, ip2)
            first.sendall(frame(20, forged) + frame(20, payload))
            assert read(s) == (20, payload)
            data = b'second-to-first'
            payload = struct.pack('<IHIHII', ip2, 9501, ip1, 9500, 17, len(data)) + data
            second.sendall(frame(20, payload))
            assert read(f) == (20, payload)
            data = bytes(range(251)) * 260 + bytes(247)
            assert len(data) == 65507
            payload = struct.pack('<IHIHII', ip2, 9501, ip1, 9500, 17, len(data)) + data
            second.sendall(frame(20, payload))
            assert read(f) == (20, payload)
            opener = struct.pack('<IHIHI', ip1, 45000, ip2, 4000, 6)
            accepter = struct.pack('<IHIHI', ip2, 4000, ip1, 45000, 6)
            first.sendall(frame(18, opener))
            assert read(s) == (18, opener)
            second.sendall(frame(19, accepter))
            assert read(f) == (19, accepter)
            payload = accepter + struct.pack('<I',4) + b'back'
            second.sendall(frame(20, payload))
            assert read(f) == (20, payload)
            payload = opener + struct.pack('<I',2) + b'go'
            first.sendall(frame(20, payload))
            assert read(s) == (20, payload)
            first.sendall(frame(21, opener + bytes(4)))
            assert read(s) == (21, opener + bytes(4))
            first.close()
            f.close()
            assert read(s) == (16, bytes(4))
            with socket.create_connection(('127.0.0.1', port), 2) as resumed, resumed.makefile('rb') as back:
                resumed.sendall(frame(0, identity))
                kind, new_identity = read(back)
                assert kind == 0 and new_identity[:16] != identity[:16] and new_identity[16:] == identity[16:]
            print('Real RoomCircuit relay with upstream RyuLDN codec and TCP sockets passed initialize, host, scan, join, sync, UDP and TCP game data in both directions, stream close and host departure')
    finally:
        server.terminate()
        logs = server.communicate(timeout=5)[0].decode()
        print(logs[:3500])
