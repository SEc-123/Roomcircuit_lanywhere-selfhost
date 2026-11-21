#!/usr/bin/env python3
"""Build an offline, loopback-only RoomCircuit staging profile without starting it."""
import argparse
import ctypes
import hashlib
import importlib.util
import shutil
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
BACKEND=HERE.parent
PROJECT=BACKEND.parent
APPLICATIONS={'mario-party-superstars':'01006fe013472000','pico-park':'01004ea00df70000'}


def command(*args, cwd=None, env=None):
    try:
        result=subprocess.run(args,cwd=cwd,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=180)
    except subprocess.TimeoutExpired:
        raise ValueError('build or key generation timed out') from None
    if result.returncode:
        # No command line or environment is echoed; stderr contains compiler errors only.
        raise ValueError('build or key generation failed: '+result.stderr.decode(errors='replace')[-1500:])


def write_private(path, data):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'w') as f:f.write(data)


def envfile(path, fields):
    for key,value in fields.items():
        if not re.fullmatch(r'[A-Z][A-Z0-9_]*',key) or any(c in str(value) for c in '\r\n\0'):
            raise ValueError('invalid environment variable')
    write_private(path,''.join(f'{key}={value}\n' for key,value in fields.items()))


def elf64(path):
    data=path.read_bytes()
    if len(data)<64 or data[:6]!=b'\x7fELF\x02\x01' or data[18:20]!=b'\x3e\x00':
        raise ValueError('build did not produce a Linux x86-64 binary')
    return hashlib.sha256(data).hexdigest()


def rename_new(source, destination):
    """Atomically publish a directory without replacing a racing destination."""
    libc=ctypes.CDLL(None,use_errno=True)
    if sys.platform == 'darwin' and hasattr(libc,'renamex_np'):
        call=libc.renamex_np
        call.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint]
        result=call(os.fsencode(source),os.fsencode(destination),4) # RENAME_EXCL
    elif sys.platform.startswith('linux') and hasattr(libc,'renameat2'):
        call=libc.renameat2
        call.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]
        result=call(-100,os.fsencode(source),-100,os.fsencode(destination),1) # RENAME_NOREPLACE
    else:
        raise OSError('atomic no-replace directory rename is unavailable')
    if result:
        error=ctypes.get_errno()
        raise OSError(error,os.strerror(error))


def prepare(root,name,source,title,pgbin,pglib=None):
    root=Path(root).resolve(strict=True)
    if not root.is_dir() or root==Path('/') or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}',name):
        raise ValueError('a non-root working directory and simple new stage name are required')
    final=root/name
    if not Path(pgbin).is_absolute() or any(c in pgbin for c in '\r\n\0"\\'):
        raise ValueError('PostgreSQL binary directory must be a safe absolute path')
    if pglib and any(not Path(lib).is_absolute() or any(c in lib for c in '\r\n\0"\\:') for lib in pglib):
        raise ValueError('PostgreSQL library directories must be safe absolute paths')
    spec=importlib.util.spec_from_file_location('rc_stage_units',HERE/'systemd/render.py')
    units=importlib.util.module_from_spec(spec);spec.loader.exec_module(units)
    units.check_stage(str(final))
    if final.exists() or final.is_symlink():
        raise ValueError('stage already exists; existing data was not changed')
    if any(c in str(final) for c in '\r\n\0"\\'):
        raise ValueError('stage path contains unsupported characters')
    source=Path(source).resolve(strict=True)
    if title not in APPLICATIONS:raise ValueError('unknown game title')
    # Temporary and final paths share a parent: rename publishes atomically.
    old_umask=os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(prefix='.rc-stage-',dir=root) as scratch:
            p=Path(scratch)/'output';p.mkdir(mode=0o700)
            for name0 in ('bin','env','state','tls','keys'): (p/name0).mkdir(mode=0o700)
            buildenv={**os.environ,'CGO_ENABLED':'0','GOOS':'linux','GOARCH':'amd64','GOWORK':'off','GOTOOLCHAIN':'local','GOPROXY':'off'}
            modules=[('username-core',BACKEND/'username-core'),('identity',PROJECT/'account-lab/identity'),
                     ('broker',BACKEND),('adapter',BACKEND/'nx-adapter'),('register-gateway',HERE/'register-gateway')]
            for executable,module in modules:
                command('go','build','-mod=readonly','-trimpath','-o',str(p/'bin'/executable),'.',cwd=module,env=buildenv)
            command('python3','-B',str(PROJECT/'backend-lab/runtime/build.py'),'--source',str(source),'--binary',str(p/'bin/nex'),'--linux')
            binary_hashes={name0:elf64(p/'bin'/name0) for name0,_ in modules}
            binary_hashes['nex']=elf64(p/'bin/nex')
            shutil.copyfile(HERE/'database/bootstrap.sh',p/'bin/pg-stage')
            (p/'bin/pg-stage').chmod(0o700)
            binary_hashes['pg-stage']=hashlib.sha256((p/'bin/pg-stage').read_bytes()).hexdigest()
            for filename in ('broker-issuer.pem','adapter-issuer.pem','na-issuer.pem'):
                command('openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(p/'keys'/filename))
            write_private(p/'keys/device-state.key',secrets.token_hex(32))
            command('openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','2',
                    '-keyout',str(p/'tls/ca.key'),'-out',str(p/'tls/ca.pem'),'-subj','/CN=RoomCircuit Staging CA',
                    '-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign')
            command('openssl','req','-newkey','rsa:2048','-nodes','-keyout',str(p/'tls/server.key'),
                    '-out',str(p/'tls/server.csr'),'-subj','/CN=127.0.0.1')
            (p/'tls/leaf.ext').write_text('basicConstraints=critical,CA:FALSE\nsubjectAltName=IP:127.0.0.1,DNS:localhost\nextendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n')
            command('openssl','x509','-req','-in',str(p/'tls/server.csr'),'-CA',str(p/'tls/ca.pem'),'-CAkey',str(p/'tls/ca.key'),
                    '-CAcreateserial','-days','2','-extfile',str(p/'tls/leaf.ext'),'-out',str(p/'tls/server.pem'))
            command('openssl','verify','-CAfile',str(p/'tls/ca.pem'),str(p/'tls/server.pem'))
            certificate=ssl._ssl._test_decode_cert(str(p/'tls/server.pem'))
            if ('IP Address','127.0.0.1') not in certificate.get('subjectAltName', ()):
                raise ValueError('local TLS certificate must identify 127.0.0.1')
            core_key,links_key,broker_key,adapter_key,ticket_key=[secrets.token_hex(32) for _ in range(5)]
            tls=lambda filename:str(final/'tls'/filename)
            keys=lambda filename:str(final/'keys'/filename)
            state=lambda filename:str(final/'state'/filename)
            core='http://127.0.0.1:39910'; broker='https://127.0.0.1:39913'; adapter='https://127.0.0.1:39915'
            pg_config={'RC_STAGE_PG_BIN':pgbin,'RC_STAGE_PG_ROOT':str(final),'RC_STAGE_PG_PORT':'39900'}
            if pglib:pg_config['LD_LIBRARY_PATH']=':'.join(pglib)
            envfile(p/'env/postgres.env',pg_config)
            envfile(p/'env/core.env',{'RC_USERNAME_CORE_KEY':core_key,'RC_USERNAME_HTTP':'127.0.0.1:39910','RC_USERNAME_GRPC':'127.0.0.1:39911'})
            envfile(p/'env/identity.env',{'RC_CORE_HTTP':core,'RC_CORE_GRPC':'127.0.0.1:39911','RC_CORE_KEY':core_key,'RC_IDENTITY_KEY':links_key,'RC_IDENTITY_LISTEN':'127.0.0.1:39912','RC_ACCOUNT_REALM':'username'})
            envfile(p/'env/broker.env',{'RC_DEVICE_ISSUER':broker,'RC_ACCOUNT_URL':core,'RC_LINKS_URL':'http://127.0.0.1:39912','RC_LINKS_KEY':links_key,'RC_DEVICE_GAME_KEY':broker_key,'RC_DEVICE_SIGNING_KEY':keys('broker-issuer.pem'),'RC_DEVICE_CERT':tls('server.pem'),'RC_DEVICE_TLS_KEY':tls('server.key'),'RC_DEVICE_LISTEN':'127.0.0.1:39913','RC_DEVICE_INTERNAL_LISTEN':'127.0.0.1:39914','RC_ACCOUNT_CORE_GRPC':'127.0.0.1:39911','RC_ACCOUNT_CORE_KEY':core_key,'RC_ACCOUNT_REALM':'username','RC_DEVICE_GRANT_STATE':state('grants.json'),'RC_DEVICE_APPLICATIONS':APPLICATIONS[title]})
            envfile(p/'env/adapter.env',{'NX_LAB_ISSUER':adapter,'NX_LAB_LISTEN':'127.0.0.1:39915','NX_LAB_INTERNAL':'127.0.0.1:39916','NX_LAB_GRANT_URL':'http://127.0.0.1:39914','NX_LAB_GRANT_KEY':broker_key,'NX_LAB_GAME_KEY':adapter_key,'NX_LAB_APPLICATION':APPLICATIONS[title],'NX_LAB_SIGNING_KEY':keys('adapter-issuer.pem'),'NX_LAB_CERT':tls('server.pem'),'NX_LAB_TLS_KEY':tls('server.key'),'NX_LAB_STATE':state('devices.enc'),'NX_LAB_STATE_KEY':keys('device-state.key'),'NX_LAB_OAUTH_REDIRECT':'roomcircuit://device-link','NX_LAB_OAUTH_CLIENT_ID':'roomcircuit-owned-fixture','NX_LAB_NA_ISSUER':adapter,'NX_LAB_NA_SIGNING_KEY':keys('na-issuer.pem'),'NX_LAB_BROWSER_CA':tls('ca.pem'),'NX_LAB_BROWSER_ORIGIN':broker,'NX_LAB_ACCOUNT_URL':core,'NX_LAB_ACCOUNT_REALM':'username'})
            envfile(p/'env/nex.env',{'NX_INTERNAL_URL':'http://127.0.0.1:39916','NX_INTERNAL_KEY':adapter_key,'RC_NEX_TICKET_KEY':ticket_key})
            envfile(p/'env/register.env',{'RC_REGISTER_LISTEN':'127.0.0.1:39917','RC_REGISTER_UPSTREAM':broker,'RC_REGISTER_ORIGIN':'https://127.0.0.1:39917','RC_REGISTER_INTERNAL_KEY':broker_key,'RC_REGISTER_CA':tls('ca.pem'),'RC_REGISTER_CERT':tls('server.pem'),'RC_REGISTER_TLS_KEY':tls('server.key')})
            (p/'profile.json').write_text(json.dumps({'schema':1,'purpose':'private loopback staging only','title':title,'application_id':APPLICATIONS[title],
                'ports':[39900,39910,39911,39912,39913,39914,39915,39916,39917,22110 if title=='mario-party-superstars' else 23080,22111 if title=='mario-party-superstars' else 23081,22112 if title=='mario-party-superstars' else 23082],
                'binary_sha256':binary_hashes,'device_enrollment_configured':False,'native_gameplay_verified':False,'certificate_days':2},indent=2)+'\n')
            units.render(str(final),title,p/'units')
            # Database initialization, systemd installation and activation remain separate.
            rename_new(p,final)
    finally:
        os.umask(old_umask)
    return final


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',required=True,help='Existing directory for the new private stage')
    p.add_argument('--name',required=True,help='Simple new stage directory name')
    p.add_argument('--nex-source',required=True,help='Pinned local Switch NEX v0.3.45 source')
    p.add_argument('--title',choices=sorted(APPLICATIONS),required=True)
    p.add_argument('--pg-bin',required=True,help='Trusted PostgreSQL 16 binary directory on the target host')
    p.add_argument('--pg-lib',action='append',help='Trusted PostgreSQL library directory on the target host; repeat if needed')
    a=p.parse_args()
    try:print('Prepared private loopback staging files:',prepare(a.root,a.name,a.nex_source,a.title,a.pg_bin,a.pg_lib))
    except (OSError,ValueError) as e:p.error(str(e))
if __name__=='__main__':main()
