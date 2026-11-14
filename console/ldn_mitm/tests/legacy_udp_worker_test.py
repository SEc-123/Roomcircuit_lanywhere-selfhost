#!/usr/bin/env python3
"""Actual worker and UDP codec with live loopback sockets and bounded accelerated timing."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'source/lanywhere_relay.cpp')
parser.add_argument('--mode', type=int, choices=range(1, 8))
args = parser.parse_args()
source = args.source.read_text()


def function(signature):
    start = source.index(signature)
    depth = 0
    for at in range(source.index('{', start), len(source)):
        if source[at] == '{':
            depth += 1
        elif source[at] == '}':
            depth -= 1
            if depth == 0:
                return source[start:at + 1]
    raise ValueError(signature)


functions = '\n'.join(function(name) for name in (
    '        bool UdpSendTo(', '        bool UdpSend(', '        ssize_t UdpReceiveFrom(',
    '        bool RecoverTransport(', '        bool RegisterUdpAgain(', '        void Worker(void *)'))
harness = (root / 'tests/legacy_udp_worker_harness.cpp.in').read_text().replace('/* ACTUAL_FUNCTIONS */', functions)
with tempfile.TemporaryDirectory(prefix='legacy-udp-worker-') as temporary:
    directory = Path(temporary)
    cpp = directory / 'worker.cpp'
    executable = directory / 'worker'
    cpp.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(root / 'source'),
        str(cpp), '-o', str(executable)], check=True, timeout=30)
    for mode in ([args.mode] if args.mode else range(1, 8)):
        subprocess.run([str(executable), str(mode)], check=True, timeout=10)
print('Actual native SHA-256', hashlib.sha256(source.encode()).hexdigest())
print('Live loopback sockets, accelerated clock, recovery socket-install seam; no Switch or public node')
