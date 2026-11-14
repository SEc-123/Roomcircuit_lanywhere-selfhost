#!/usr/bin/env python3
"""Exercise the update packager with isolated keys, exact file map and malformed inputs."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parent.parent


def nro(version):
    data = bytearray(0x80 + 0x38 + 0x4000)
    data[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', data, 0x18, 0x80)
    data[0x80:0x84] = b'ASET'
    struct.pack_into('<QQ', data, 0x98, 0x38, 0x4000)
    raw = version.encode('ascii')
    data[0x80 + 0x38 + 0x3060:0x80 + 0x38 + 0x3060 + len(raw)] = raw
    return data


def main():
    with tempfile.TemporaryDirectory(prefix='update-sign-package-') as temporary:
        root = Path(temporary)
        (root / 'scripts').mkdir(); (root / 'common').mkdir()
        shutil.copy2(ROOT / 'scripts/make-online-update.py', root / 'scripts/make-online-update.py')
        shutil.copy2(ROOT / 'common/update_format.h', root / 'common/update_format.h')
        (root / 'package.json').write_text('{"version":"0.7.1"}')
        key = root / 'test-key.pem'
        keygen = '''const crypto=require('crypto'),fs=require('fs');
const keys=crypto.generateKeyPairSync('ed25519');
fs.writeFileSync(process.argv[1],keys.privateKey.export({type:'pkcs8',format:'pem'}),{mode:0o600});
process.stdout.write(keys.publicKey.export({type:'spki',format:'der'}).subarray(-32).toString('hex'));'''
        public = subprocess.check_output(['node', '-e', keygen, str(key)], text=True).strip()
        (root / 'common/update_public_key.h').write_text('static const unsigned char key[]={' +
            ','.join('0x' + public[i:i + 2] for i in range(0, 64, 2)) + '};')
        names = re.findall(r'^\s*"([^"]+)",?$', (root / 'common/update_format.h').read_text(), re.M)
        files = {name: b'fixture' for name in names}
        for name in names:
            if name.endswith(('.nro', '.ovl')): files[name] = nro('0.7.1')
            if name.endswith('boot2.flag'): files[name] = b''
            if name.endswith('exefs.nsp'): files[name] = b'PFS0' + b'fixture'
        archive = root / 'player.zip'
        def write_zip(values):
            with zipfile.ZipFile(archive, 'w') as z:
                for name, payload in values.items(): z.writestr(name, payload)
        write_zip(files)
        args = ['python3', root / 'scripts/make-online-update.py', '--sd-zip', archive, '--key', key,
            '--output-dir', root / 'out', '--notes-en', 'Online update test', '--notes-zh', '在线更新测试']
        def run(ok=True):
            p = subprocess.run(args, capture_output=True, text=True)
            assert (p.returncode == 0) == ok, p.stdout + p.stderr
            return json.loads(p.stdout) if ok else p.stderr
        result = run(); result_again = run(); assert result == result_again
        manifest = (root / 'out/current.manifest').read_bytes()
        compressed = Path(result['package']).read_bytes()
        assert len(manifest) == 512 and manifest[:4] == b'LNU1'
        assert hashlib.sha256(compressed).digest() == manifest[36:68]
        raw = zlib.decompress(compressed)
        assert len(raw) == struct.unpack_from('<I', manifest, 28)[0]
        assert struct.unpack_from('<II', raw, 4) == (1, len(names))
        offset = 12
        for index, name in enumerate(names):
            actual, size = struct.unpack_from('<II', raw, offset)
            digest = raw[offset + 8:offset + 40]; data = raw[offset + 40:offset + 40 + size]
            assert actual == index and data == files[name] and digest == hashlib.sha256(data).digest()
            offset += 40 + size
        assert offset == len(raw)
        verify = '''const fs=require('fs'),crypto=require('crypto');const m=fs.readFileSync(process.argv[1]);
const key=crypto.createPublicKey({key:Buffer.from('302a300506032b6570032100'+process.argv[2],'hex'),format:'der',type:'spki'});
if(!crypto.verify(null,m.subarray(0,448),key,m.subarray(448)))process.exit(1);'''
        subprocess.run(['node', '-e', verify, str(root / 'out/current.manifest'), public], check=True)
        bad = dict(files); bad['lanywhere/config.ini'] = b'private settings'; write_zip(bad); assert 'Unexpected path' in run(False)
        bad = dict(files); bad[names[-1]] = b'wrong'; write_zip(bad); assert 'Invalid sysmodule' in run(False)
        bad = dict(files); bad['switch/lanywhere.nro'] = nro('0.7.0'); write_zip(bad); assert 'differs' in run(False)
        write_zip(files); key.chmod(0o644); assert 'mode 0600' in run(False); key.chmod(0o600)
        subprocess.check_output(['node', '-e', keygen, str(key)]); assert 'does not match' in run(False)
        assert (root / 'out/current.manifest').read_bytes() == manifest
        print('Signed update package: exact destinations, signature/hash, idempotence, private-path/version/key rejection passed')


if __name__ == '__main__':
    main()
