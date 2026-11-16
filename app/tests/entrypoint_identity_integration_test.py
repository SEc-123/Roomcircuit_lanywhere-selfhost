#!/usr/bin/env python3
"""Verify the actual SD signer against the actual app's RCI1 verifier."""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('update_fixture', ROOT / 'scripts/test-online-update-package.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
node = os.environ.get('NODE', 'node')
keygen = '''const c=require('crypto'),f=require('fs');const k=c.generateKeyPairSync('ed25519');
f.writeFileSync(process.argv[1],k.privateKey.export({type:'pkcs8',format:'pem'}),{mode:0o600});
process.stdout.write(k.publicKey.export({type:'spki',format:'der'}).subarray(-32).toString('hex'));'''

with tempfile.TemporaryDirectory(prefix='roomcircuit-identity-integration-') as temporary:
    root = Path(temporary)
    pack = root / 'pack'; (pack / 'scripts').mkdir(parents=True); (pack / 'common').mkdir()
    for name in ('make-online-update.py','sign-entrypoints.py'):
        shutil.copy2(ROOT / 'scripts' / name, pack / 'scripts' / name)
    shutil.copy2(ROOT / 'common/update_format.h', pack / 'common/update_format.h')
    (pack / 'package.json').write_text('{"version":"0.9.0"}')
    def make_key(name):
        key = root / name
        public = subprocess.check_output([node, '-e', keygen, str(key)], text=True).strip()
        (pack / 'common/update_public_key.h').write_text('static const unsigned char key[]={' +
            ','.join('0x'+public[i:i+2] for i in range(0, 64, 2)) + '};')
        return key, public
    key, public = make_key('one.pem')
    names = re.findall(r'^\s*"([^"]+)",?$', (pack / 'common/update_format.h').read_text(), re.M)
    def target(name):
        if name == 'switch/lanywhere.nro': return 'switch/RoomCircuit/RoomCircuit.nro'
        if name == 'switch/.overlays/lanywhere.ovl': return 'switch/.overlays/RoomCircuit.ovl'
        return 'switch/RoomCircuit/' + name[len('lanywhere/'):] if name.startswith('lanywhere/') else name
    data = {}
    for name in names:
        value = b'fixture'
        if name.endswith(('.nro','.ovl')): value = bytes(fixture.nro('0.9.0'))
        if name.endswith('boot2.flag'): value = b''
        if name.endswith('exefs.nsp'): value = b'PFS0' + b'fixture'
        data[target(name)] = value
    unsigned = root / 'unsigned.zip'
    with zipfile.ZipFile(unsigned, 'w') as archive:
        for name, value in data.items(): archive.writestr(name, value)
    def sign(key, filename):
        output = root / filename
        subprocess.run(['python3', str(pack/'scripts/sign-entrypoints.py'), '--sd-zip', str(unsigned),
                        '--output', str(output), '--key', str(key), '--node', node], check=True, capture_output=True)
        return output
    signed = sign(key, 'signed.zip')
    other_key, _ = make_key('other.pem')
    wrong_key = sign(other_key, 'wrong-key.zip')
    # Verifier uses the *first* key, independent of the signer process.
    key_bytes = ','.join('0x'+public[i:i+2] for i in range(0,64,2))
    c_file = root / 'test.c'; executable = root / 'test'
    c_file.write_text('''#include <assert.h>\n#include <stdio.h>\nstatic const unsigned char key[32]={''' + key_bytes + '''};
#define UPDATE_VERIFY_PUBLIC_KEY key
#include "update_core.c"
int main(int argc,char **argv){assert(argc==4);update_record_renamed_ownership(argv[1],argv[2]);
 char path[PATH_MAX];bool found;assert(path_join(path,argv[1],BRIDGE_OWNER)&&regular_or_missing(path,&found));
 assert(found == (argv[3][0]!='0'));
 if(argv[3][0]=='2') for(unsigned i=0;i<2;i++) { char target[PATH_MAX]; assert(path_join(target,argv[1],BridgePaths[i]) && bridge_owned_matches(argv[1],i,target)); }
 puts(found?"recognized signed entries":"rejected untrusted entries");}
''')
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-std=c11','-D_POSIX_C_SOURCE=200809L',
                   '-I'+str(ROOT/'app/source'),str(c_file),'-lsodium','-lz','-o',str(executable)],check=True)
    for name, archive, version, changed, expected in [
        ('valid', signed, '0.9.0', None, '1'),
        ('wrong-key', wrong_key, '0.9.0', None, '0'),
        ('wrong-version', signed, '0.8.9', None, '0'),
        ('app-changed', signed, '0.9.0', 'switch/RoomCircuit/RoomCircuit.nro', '0'),
        ('overlay-changed', signed, '0.9.0', 'switch/.overlays/RoomCircuit.ovl', '0'),
        ('signature-changed', signed, '0.9.0', 'switch/RoomCircuit/.entrypoints-signed', '0'),
    ]:
        sd = root / name; sd.mkdir()
        with zipfile.ZipFile(archive) as z:
            for member in ('switch/RoomCircuit/RoomCircuit.nro','switch/.overlays/RoomCircuit.ovl',
                           'switch/RoomCircuit/.entrypoints-signed'):
                output = sd/member; output.parent.mkdir(parents=True,exist_ok=True)
                value = bytearray(z.read(member))
                if member == changed: value[-1] ^= 1
                output.write_bytes(value)
        subprocess.run([str(executable),str(sd),version,expected],check=True,timeout=10)
    # Manual signed upgrade over an existing owner must refresh that marker.
    # The invalid B proof must preserve A's owner until B is verified.
    (pack / 'common/update_public_key.h').write_text('static const unsigned char key[]={' + key_bytes + '};')
    next_unsigned = root / 'next-unsigned.zip'
    with zipfile.ZipFile(next_unsigned, 'w') as archive:
        for name, value in data.items():
            archive.writestr(name, bytes(fixture.nro('0.9.1')) if name.endswith(('.nro','.ovl')) else value)
    next_signed = root / 'next-signed.zip'
    subprocess.run(['python3',str(pack/'scripts/sign-entrypoints.py'),'--sd-zip',str(next_unsigned),
                    '--output',str(next_signed),'--key',str(key),'--node',node,'--version','0.9.1'],check=True,capture_output=True)
    sd = root / 'manual-upgrade'; sd.mkdir()
    members = ('switch/RoomCircuit/RoomCircuit.nro','switch/.overlays/RoomCircuit.ovl',
               'switch/RoomCircuit/.entrypoints-signed')
    def extract(archive, corrupt=False):
        with zipfile.ZipFile(archive) as z:
            for member in members:
                output = sd/member; output.parent.mkdir(parents=True,exist_ok=True)
                value = bytearray(z.read(member))
                if corrupt and member.endswith('.entrypoints-signed'): value[-1] ^= 1
                output.write_bytes(value)
    extract(signed)
    subprocess.run([str(executable),str(sd),'0.9.0','2'],check=True,timeout=10)
    owner = sd/'switch/RoomCircuit/.entrypoints-owned-v1'; old_owner = owner.read_bytes()
    extract(next_signed, corrupt=True)
    subprocess.run([str(executable),str(sd),'0.9.1','1'],check=True,timeout=10)
    assert owner.read_bytes() == old_owner
    extract(next_signed)
    subprocess.run([str(executable),str(sd),'0.9.1','2'],check=True,timeout=10)
    assert owner.read_bytes() != old_owner
    print('PASS real RCI1 signer and app verifier: valid, wrong key/version, tampering and signed manual upgrade')
