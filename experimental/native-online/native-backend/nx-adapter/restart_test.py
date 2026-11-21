#!/usr/bin/env python3
"""Replay two approved devices across a real adapter process restart."""
import argparse
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile

import smoke


def local_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, help='Use a prebuilt executable for this host.')
    args = parser.parse_args()
    upstream = smoke.ROOT.parent / '.runtime'
    with tempfile.TemporaryDirectory(prefix='roomcircuit-nx-restart-') as temp:
        lab = Path(temp)
        if args.binary:
            binary = args.binary.resolve(strict=True)
        else:
            binary = lab / 'nx-adapter'
            subprocess.run(['go', 'build', '-mod=readonly', '-trimpath', '-o', str(binary), '.'], cwd=smoke.ROOT, check=True)
        issuer_key = lab / 'issuer-key.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048',
                        '-out', str(issuer_key)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        smoke.LAB = lab
        game_key = lab / 'game-key'
        state_key = lab / 'device-key'
        for key in (game_key, state_key):
            fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as out:
                out.write(secrets.token_hex(32))
        # Reserve distinct local ports first. The JWT issuer need not resolve to
        # this test host and is never contacted by the fixture.
        public_port, private_port = local_port(), local_port()
        while public_port == private_port:
            private_port = local_port()
        smoke.NX = f'https://127.0.0.1:{public_port}'
        smoke.PRIVATE = f'http://127.0.0.1:{private_port}'
        issuer = 'https://roomcircuit.example.test'
        env = {**os.environ, 'NX_LAB_ISSUER': issuer, 'NX_LAB_LISTEN': f'127.0.0.1:{public_port}',
               'NX_LAB_INTERNAL': f'127.0.0.1:{private_port}', 'NX_LAB_GRANT_URL': 'http://127.0.0.1:39281',
               'NX_LAB_GRANT_KEY': (upstream / 'game-key').read_text().strip(), 'NX_LAB_GAME_KEY': game_key.read_text(),
               'NX_LAB_APPLICATION': smoke.APP, 'NX_LAB_SIGNING_KEY': str(issuer_key),
               'NX_LAB_CERT': str(upstream / 'server.pem'), 'NX_LAB_TLS_KEY': str(upstream / 'server-key.pem'),
               'NX_LAB_STATE': str(lab / 'devices.enc'), 'NX_LAB_STATE_KEY': str(state_key)}

        def start():
            return subprocess.Popen([str(binary)], env=env)

        def stop(process):
            process.terminate()
            process.wait(timeout=6)

        process = start()
        def restart():
            nonlocal process
            before = process.pid
            stop(process)
            process = start()
            assert process.pid != before

        try:
            smoke.main(restart, issuer, smoke.ROOT / 'restart-receipt.json')
        finally:
            stop(process)


if __name__ == '__main__':
    main()
