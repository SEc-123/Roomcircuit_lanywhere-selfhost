#!/usr/bin/env python3
"""Create local development TLS assets for an explicitly selected own CA.

This never starts a listener, modifies the Switch, or contacts any server.
"""
import argparse
import importlib.util
import ipaddress
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('rc_preflight', ROOT/'preflight'/'check.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
spec2 = importlib.util.spec_from_file_location('rc_inputs', ROOT/'build-inputs'/'prepare.py')
inputs = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(inputs)


def run(*args):
    result = subprocess.run(args, capture_output=True)
    if result.returncode:
        raise ValueError('OpenSSL rejected the supplied CA or generated certificate')
    return result.stdout


def create(ca_path, key_path, address, names, destination):
    ip = ipaddress.IPv4Address(address)
    if ip.is_unspecified or ip.is_loopback or ip.is_multicast or ip.is_link_local or ip.is_reserved:
        raise ValueError('a usable non-local IPv4 address is required')
    if not names or len(names) != len(set(names)) or any(n not in module.ALIASES for n in names):
        raise ValueError('select distinct aliases from the supported list')
    destination = Path(destination)
    if destination.exists():
        raise ValueError('destination already exists; existing material was not changed')
    destination.parent.mkdir(parents=True, exist_ok=True)
    cert = Path(ca_path).read_bytes()
    # Never print or copy the private key into the resulting package.
    cert_key = run('openssl','x509','-in',str(ca_path),'-pubkey','-noout')
    private_pub = run('openssl','pkey','-in',str(key_path),'-pubout')
    if cert_key != private_pub:
        raise ValueError('CA certificate and private key do not match')
    with tempfile.TemporaryDirectory(prefix='.rc-tls-', dir=destination.parent) as d:
        tmp = Path(d)
        der = tmp/'ca.der'
        der.write_bytes(run('openssl','x509','-in',str(ca_path),'-outform','DER'))
        inputs.prepare(der,tmp/'validate',str(ip))
        work=tmp/'result';work.mkdir(mode=0o700)
        shutil.copyfile(der,work/'ca.der')
        (work/'ca.pem').write_bytes(cert)
        run('openssl','req','-newkey','rsa:2048','-nodes','-keyout',str(work/'server.key'),
            '-out',str(tmp/'server.csr'),'-subj','/CN=RoomCircuit independent service')
        (work/'server.key').chmod(0o600)
        # Use local copies so OpenSSL's serial creation never touches the source CA.
        (tmp/'ca.pem').write_bytes(cert)
        shutil.copyfile(key_path,tmp/'ca.key');(tmp/'ca.key').chmod(0o600)
        san = 'IP:'+str(ip)+''.join(',DNS:'+name for name in names)
        (tmp/'leaf.ext').write_text('basicConstraints=critical,CA:FALSE\nsubjectAltName='+san+
            '\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature,keyEncipherment\n'
            'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n')
        run('openssl','x509','-req','-in',str(tmp/'server.csr'),'-CA',str(tmp/'ca.pem'),
            '-CAkey',str(tmp/'ca.key'),'-CAcreateserial','-days','7','-extfile',str(tmp/'leaf.ext'),
            '-out',str(work/'server.pem'))
        run('openssl','verify','-CAfile',str(tmp/'ca.pem'),str(work/'server.pem'))
        (work/'aliases.txt').write_text('\n'.join(names)+'\n')
        (work/'address.txt').write_text(str(ip)+'\n')
        work.rename(destination)
    return destination


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ca',required=True,help='owned CA public certificate (PEM)')
    p.add_argument('--ca-key',required=True,help='matching owned CA private key (never packaged)')
    p.add_argument('--address',required=True)
    p.add_argument('--name',action='append',required=True,choices=sorted(module.ALIASES))
    p.add_argument('--output',required=True)
    a=p.parse_args()
    try: print('Prepared local test assets:',create(a.ca,a.ca_key,a.address,a.name,a.output))
    except (OSError,ValueError,subprocess.SubprocessError) as exc:p.error(str(exc))

if __name__=='__main__':main()
