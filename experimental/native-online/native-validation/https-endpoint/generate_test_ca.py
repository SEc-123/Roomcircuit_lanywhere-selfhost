#!/usr/bin/env python3
"""Generate an isolated test CA constrained to explicit SDK DNS names.

No real device identity is involved. Nothing is deployed or installed.
"""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('rc_preflight', HERE.parent/'preflight'/'check.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


def create(names, destination):
    if not names or len(names) != len(set(names)) or any(n not in preflight.ALIASES for n in names):
        raise ValueError('select distinct SDK aliases from the supported list')
    destination = Path(destination)
    if destination.exists():
        raise ValueError('destination already exists; existing material was not changed')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.rc-ca-', dir=destination.parent) as d:
        work = Path(d)/'result'
        work.mkdir(mode=0o700)
        constraints = 'nameConstraints=critical,'+','.join('permitted;DNS:'+name for name in names)
        result = subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','30',
            '-keyout',str(work/'ca.key'),'-out',str(work/'ca.pem'),'-subj','/CN=RoomCircuit Test CA',
            '-addext','basicConstraints=critical,CA:TRUE,pathlen:0',
            '-addext','keyUsage=critical,keyCertSign,cRLSign',
            '-addext','subjectKeyIdentifier=hash','-addext',constraints],capture_output=True)
        if result.returncode:
            raise ValueError('could not create constrained CA')
        (work/'ca.key').chmod(0o600)
        (work/'allowed-dns.txt').write_text('\n'.join(names)+'\n')
        work.rename(destination)
    return destination


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--name',action='append',required=True,choices=sorted(preflight.ALIASES))
    p.add_argument('--output',required=True)
    a=p.parse_args()
    try:print('Prepared local test CA:',create(a.name,a.output))
    except (OSError,ValueError) as exc:p.error(str(exc))

if __name__=='__main__':main()
