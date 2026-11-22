#!/usr/bin/env python3
"""Probe an explicitly supplied endpoint's TLS for a fixed set of SDK hostnames.

This does not activate DNS, enroll a device, or send user credentials. It checks
server TLS only; it cannot establish that HOS account or game services work.
"""
import argparse
import ipaddress
import json
import socket
import ssl
import sys

ALIASES = {
    'accounts.nintendo.com',
    'api.accounts.nintendo.com',
    'e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com',
    'm-lp1.baas.nintendo.com',
    'capi.lp1.op2.nintendo.net',
    'dauth-lp1.ndas.srv.nintendo.net',
    'dcert-lp1.ndas.srv.nintendo.net',
    'aauth-lp1.ndas.srv.nintendo.net',
    'aauth.hac.lp1.ndas.srv.nintendo.net',
    'g2c3c9a00-lp1.s.n.srv.nintendo.net',
    'g28abaa00-lp1.s.n.srv.nintendo.net',
}


def probe(address, port, names, ca_file, timeout):
    ip = str(ipaddress.IPv4Address(address))
    if not names or any(name not in ALIASES for name in names) or len(set(names)) != len(names):
        raise ValueError('names must be distinct supported aliases')
    if not (1 <= port <= 65535) or not (0 < timeout <= 15):
        raise ValueError('invalid port or timeout')
    context = ssl.create_default_context(cafile=ca_file)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    results = []
    for name in names:
        try:
            with socket.create_connection((ip, port), timeout=timeout) as raw:
                raw.settimeout(timeout)
                with context.wrap_socket(raw, server_hostname=name) as connection:
                    results.append({'hostname': name, 'tls_verified': True,
                                    'tls_version': connection.version()})
        except ssl.SSLCertVerificationError as exc:
            results.append({'hostname': name, 'tls_verified': False,
                            'stage': 'certificate', 'error': str(exc)})
        except (OSError, ssl.SSLError) as exc:
            results.append({'hostname': name, 'tls_verified': False,
                            'stage': 'connection_or_handshake', 'error': str(exc)})
    return {'address': ip, 'port': port, 'checks': results,
            'scope': 'server TLS only; no account, device enrollment or game validation',
            'all_tls_verified': all(item['tls_verified'] for item in results)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', required=True, help='explicitly authorized server IPv4 address')
    parser.add_argument('--ca', required=True, help='owned CA in PEM format')
    parser.add_argument('--name', action='append', required=True, choices=sorted(ALIASES))
    parser.add_argument('--port', type=int, default=443)
    parser.add_argument('--timeout', type=float, default=3)
    args = parser.parse_args()
    try:
        result = probe(args.address, args.port, args.name, args.ca, args.timeout)
    except (ValueError, OSError, ssl.SSLError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))
    return 0 if result['all_tls_verified'] else 1


if __name__ == '__main__':
    sys.exit(main())
