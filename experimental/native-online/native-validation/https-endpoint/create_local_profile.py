#!/usr/bin/env python3
"""Write a loopback-only SDK account routing template. Does not start services."""
import argparse
import ipaddress
import json
from pathlib import Path
import re
import subprocess
import tempfile

SDK_ROUTES = {
    'accounts.nintendo.com': 'network account browser and issuer',
    'api.accounts.nintendo.com': 'network account API',
    'e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com': 'BAAS and BAAS keys',
    'capi.lp1.op2.nintendo.net': 'realm membership',
    'dauth-lp1.ndas.srv.nintendo.net': 'device authentication with client certificate',
    'dcert-lp1.ndas.srv.nintendo.net': 'device public keys',
    'aauth-lp1.ndas.srv.nintendo.net': 'application authentication and keys',
}
UNSUPPORTED = {
    'm-lp1.baas.nintendo.com': 'newer system account endpoint',
    'aauth.hac.lp1.ndas.srv.nintendo.net': 'newer system application-auth endpoint',
    'g2c3c9a00-lp1.s.n.srv.nintendo.net': 'PICO PARK game listener not configured',
    'g28abaa00-lp1.s.n.srv.nintendo.net': 'Mario Party game listener not configured',
}


def create(assets, destination, frontend, backend, internal):
    assets, destination = Path(assets).resolve(strict=True), Path(destination)
    for value in (frontend, backend, internal):
        if not 1 <= value <= 65535:
            raise ValueError('each loopback port must be in range 1–65535')
    if len({frontend, backend, internal}) != 3:
        raise ValueError('loopback ports must differ')
    if destination.exists():
        raise ValueError('destination already exists')
    expected = set(SDK_ROUTES)
    listed = (assets/'aliases.txt').read_text().splitlines()
    if set(listed) != expected or len(listed) != len(expected):
        raise ValueError('endpoint certificate must be prepared for exactly the seven SDK account aliases')
    if not all((assets/name).is_file() for name in ('ca.pem', 'ca.der', 'server.pem', 'server.key')):
        raise ValueError('prepared endpoint certificate, key and CA are required')
    if (assets/'server.key').stat().st_mode & 0o077:
        raise ValueError('server private key must be owner-only')
    def openssl(*args):
        result=subprocess.run(['openssl',*args],capture_output=True)
        if result.returncode:
            raise ValueError('endpoint certificate or key validation failed')
        return result.stdout
    ca_der=openssl('x509','-in',str(assets/'ca.pem'),'-outform','DER')
    if ca_der != (assets/'ca.der').read_bytes():
        raise ValueError('PEM and DER CA certificates do not match')
    cert_key=openssl('x509','-in',str(assets/'server.pem'),'-pubkey','-noout')
    private_key=openssl('pkey','-in',str(assets/'server.key'),'-pubout')
    if cert_key != private_key:
        raise ValueError('endpoint certificate and private key do not match')
    openssl('verify','-x509_strict','-CAfile',str(assets/'ca.pem'),str(assets/'server.pem'))
    address=str(ipaddress.IPv4Address((assets/'address.txt').read_text().strip()))
    detail=openssl('x509','-in',str(assets/'server.pem'),'-noout','-text').decode('utf-8')
    match=re.search(r'X509v3 Subject Alternative Name[^\n]*\n\s*([^\n]+)',detail)
    wanted={*(f'DNS:{name}' for name in expected),f'IP Address:{address}'}
    if not match or {item.strip() for item in match.group(1).split(',')} != wanted:
        raise ValueError('endpoint SAN must contain exactly the configured SDK aliases and IPv4')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.rc-profile-', dir=destination.parent) as d:
        p=Path(d)/'result';p.mkdir(mode=0o700)
        config={'listen':f'127.0.0.1:{frontend}',
                'routes':{name:{'address':f'127.0.0.1:{backend}'} for name in SDK_ROUTES}}
        (p/'ingress.json').write_text(json.dumps(config,indent=2)+'\n')
        # No device identity or secret is provided. Empty enrollment is a deliberate
        # fail-closed state: the adapter refuses to start with this example unchanged.
        env={
          'NX_LAB_LISTEN':f'127.0.0.1:{backend}',
          'NX_LAB_INTERNAL':f'127.0.0.1:{internal}',
          'NX_LAB_ISSUER':'https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com',
          'NX_LAB_BAAS_KEY_ORIGIN':'https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com',
          'NX_LAB_NA_ISSUER':'https://accounts.nintendo.com',
          'NX_LAB_DEVICE_HOST':'dauth-lp1.ndas.srv.nintendo.net',
          'NX_LAB_DCERT_ORIGIN':'https://dcert-lp1.ndas.srv.nintendo.net',
          'NX_LAB_AAUTH_ORIGIN':'https://aauth-lp1.ndas.srv.nintendo.net',
          'NX_LAB_AAUTH_KEY_ORIGIN':'https://aauth-lp1.ndas.srv.nintendo.net',
          'NX_LAB_ACCOUNT_REALM':'username',
          'NX_LAB_OAUTH_CLIENT_ID':'6ffd70c434d303c8',
          'NX_LAB_OAUTH_REDIRECT':'nintendo://account.nx.sys',
          'NX_LAB_CERT':str(assets/'server.pem'),
          'NX_LAB_TLS_KEY':str(assets/'server.key'),
          'NX_LAB_ENROLLED_SPKI':'',
          'NX_LAB_TITLE_OAUTH_CLIENT_ID':'',
        }
        # JSON is a non-executable template and cannot accidentally start a service.
        (p/'adapter-env.template.json').write_text(json.dumps(env,indent=2)+'\n')
        (p/'status.json').write_text(json.dumps({'sdk_account_aliases':SDK_ROUTES,'not_routed':UNSUPPORTED,
             'device_enrollment_configured':False,'game_auth_listener_configured':False,
             'username_registration_publicly_exposed':False,'not_for_production':True},indent=2)+'\n')
        p.rename(destination)
    return destination


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets',required=True)
    p.add_argument('--output',required=True)
    p.add_argument('--frontend-port',type=int,default=39443)
    p.add_argument('--backend-port',type=int,default=39282)
    p.add_argument('--internal-port',type=int,default=39283)
    a=p.parse_args()
    try:print('Prepared loopback-only template:',create(a.assets,a.output,a.frontend_port,a.backend_port,a.internal_port))
    except (OSError,ValueError) as exc:p.error(str(exc))

if __name__=='__main__':main()
