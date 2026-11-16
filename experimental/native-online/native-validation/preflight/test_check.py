#!/usr/bin/env python3
import contextlib
import ipaddress
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest

from check import probe

HOST = 'accounts.nintendo.com'
WRONG = 'api.accounts.nintendo.com'

class Server:
    def __init__(self, cert, key):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(.2)
        self.stop = threading.Event()
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(cert), str(key))
        self.thread = threading.Thread(target=self.run)
        self.thread.start()
    def run(self):
        while not self.stop.is_set():
            try:
                sock, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                sock.settimeout(2)
                with self.ctx.wrap_socket(sock, server_side=True):
                    pass
            except (OSError, ssl.SSLError):
                sock.close()
    def close(self):
        self.stop.set()
        self.thread.join(3)
        self.listener.close()

class Checks(unittest.TestCase):
    def test_strict_trust_and_name(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-days', '1', '-keyout', str(p/'ca.key'), '-out', str(p/'ca.pem'),
                '-subj', '/CN=Test CA', '-addext', 'basicConstraints=critical,CA:TRUE'],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['openssl', 'req', '-newkey', 'rsa:2048', '-nodes',
                '-keyout', str(p/'leaf.key'), '-out', str(p/'leaf.csr'), '-subj', '/CN='+HOST],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            (p/'ext').write_text('subjectAltName=DNS:'+HOST+'\nextendedKeyUsage=serverAuth\n')
            subprocess.run(['openssl', 'x509', '-req', '-in', str(p/'leaf.csr'), '-CA', str(p/'ca.pem'),
                '-CAkey', str(p/'ca.key'), '-CAcreateserial', '-days', '1', '-extfile', str(p/'ext'),
                '-out', str(p/'leaf.pem')], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            s = Server(p/'leaf.pem', p/'leaf.key')
            self.addCleanup(s.close)
            port = s.listener.getsockname()[1]
            good = probe('127.0.0.1', port, [HOST], str(p/'ca.pem'), 2)
            self.assertTrue(good['all_tls_verified'])
            wrong = probe('127.0.0.1', port, [WRONG], str(p/'ca.pem'), 2)
            self.assertFalse(wrong['all_tls_verified'])
            self.assertEqual(wrong['checks'][0]['stage'], 'certificate')
            untrusted = probe('127.0.0.1', port, [HOST], None, 2)
            self.assertFalse(untrusted['all_tls_verified'])
            self.assertEqual(untrusted['checks'][0]['stage'], 'certificate')
            for kwargs in [dict(address='example.com',port=port,names=[HOST]),
                           dict(address='127.0.0.1',port=0,names=[HOST]),
                           dict(address='127.0.0.1',port=port,names=['example.com']),
                           dict(address='127.0.0.1',port=port,names=[HOST, HOST])]:
                with self.assertRaises(ValueError):
                    probe(ca_file=str(p/'ca.pem'), timeout=2, **kwargs)

if __name__ == '__main__':
    unittest.main(verbosity=2)
