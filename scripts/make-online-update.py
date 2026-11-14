#!/usr/bin/env python3
"""Create a signed LANywhere online-update manifest and immutable compressed package."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parent.parent
MAX_RAW = 16 * 1024 * 1024
MAX_COMPRESSED = 8 * 1024 * 1024


def managed_files():
    header = (ROOT / 'common/update_format.h').read_text()
    paths = re.findall(r'^\s*"([^"]+)",?$', header, re.MULTILINE)
    count = int(re.search(r'#define UPDATE_FILE_COUNT (\d+)u', header)[1])
    if len(paths) != count or len(set(paths)) != count:
        raise ValueError('Invalid managed-file map')
    return paths


def public_key():
    header = (ROOT / 'common/update_public_key.h').read_text()
    values = re.findall(r'0x([0-9a-fA-F]{2})\b', header)
    if len(values) != 32:
        raise ValueError('Invalid compiled updater public key')
    return bytes.fromhex(''.join(values))


def note(value):
    encoded = value.encode('utf-8')
    if not encoded or len(encoded) >= 160 or any(ord(c) < 32 for c in value):
        raise ValueError('Each release note must be one nonempty line under 160 UTF-8 bytes')
    return encoded.ljust(160, b'\0')


def version_numbers(value):
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', value):
        raise ValueError('Use a stable major.minor.patch version')
    numbers = tuple(int(part) for part in value.split('.'))
    if max(numbers) > 65535 or numbers == (0, 0, 0):
        raise ValueError('Version components must be 0..65535 with a nonzero release')
    return numbers


def verify_nro_version(data, expected):
    if len(data) < 0x80 or data[0x10:0x14] != b'NRO0':
        raise ValueError('Invalid NRO in update')
    size, = struct.unpack_from('<I', data, 0x18)
    if data[size:size + 4] != b'ASET' or size + 0x28 > len(data):
        raise ValueError('NRO metadata missing')
    offset, length = struct.unpack_from('<QQ', data, size + 0x18)
    start = size + offset
    if length != 0x4000 or start + length > len(data):
        raise ValueError('Invalid NACP in update')
    actual = data[start + 0x3060:start + 0x3070].split(b'\0', 1)[0].decode('ascii')
    if actual != expected:
        raise ValueError(f'NRO version {actual} differs from requested update {expected}')


def package(sd_zip, version):
    names = managed_files()
    raw = bytearray(b'LNUB' + struct.pack('<II', 1, len(names)))
    with zipfile.ZipFile(sd_zip) as archive:
        entries = archive.infolist()
        if len({entry.filename for entry in entries}) != len(entries):
            raise ValueError('Duplicate update ZIP entries')
        allowed = set(names) | {'README.txt', 'GAMES.txt'}
        if any(entry.filename not in allowed or stat.S_ISLNK(entry.external_attr >> 16) for entry in entries):
            raise ValueError('Unexpected path or symlink in update ZIP')
        if sum(entry.file_size for entry in entries) > MAX_RAW:
            raise ValueError('Expanded update exceeds size limit')
        if archive.testzip() is not None:
            raise ValueError('Invalid ZIP checksum')
        for index, name in enumerate(names):
            data = archive.read(name)
            if name.endswith(('.nro', '.ovl')):
                verify_nro_version(data, version)
            if name.endswith('boot2.flag') and data:
                raise ValueError('Boot flag must be empty')
            if name.endswith('exefs.nsp') and data[:4] != b'PFS0':
                raise ValueError('Invalid sysmodule package')
            raw += struct.pack('<II', index, len(data)) + hashlib.sha256(data).digest() + data
    if len(raw) > MAX_RAW:
        raise ValueError('Expanded update exceeds size limit')
    compressed = zlib.compress(raw, 9)
    if not compressed or len(compressed) > MAX_COMPRESSED:
        raise ValueError('Compressed update exceeds size limit')
    return bytes(raw), compressed, len(names)


def sign(body, key, node):
    if key.is_symlink() or not key.is_file() or key.stat().st_mode & 0o077:
        raise ValueError('Signing key must be a private regular file with mode 0600')
    # Only the path is passed as an argument. The private key stays inside the
    # signing process. Verify the signature against the client key before output.
    script = '''
const fs = require('node:fs'), crypto = require('node:crypto');
const key = crypto.createPrivateKey(fs.readFileSync(process.argv[1]));
if (key.asymmetricKeyType !== 'ed25519') throw Error('Expected Ed25519 signing key');
const body = fs.readFileSync(0);
const signature = crypto.sign(null, body, key);
const publicDer = Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(process.argv[2], 'hex')]);
const pub = crypto.createPublicKey({key:publicDer, format:'der', type:'spki'});
if (!crypto.verify(null, body, pub, signature)) throw Error('Signing key does not match compiled updater key');
process.stdout.write(signature);
'''
    result = subprocess.run([node, '-e', script, str(key), public_key().hex()], input=body,
                            capture_output=True, check=False)
    if result.returncode or len(result.stdout) != 64:
        raise ValueError('Update signing failed or signing key does not match the client')
    return result.stdout


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.update-', dir=path.parent, delete=False) as temp:
        staging = Path(temp.name)
        temp.write(data)
        temp.flush()
        os.fsync(temp.fileno())
    try:
        staging.chmod(0o644)
        os.replace(staging, path)
        descriptor = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    finally:
        staging.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sd-zip', type=Path, required=True)
    parser.add_argument('--key', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--version', default=json.loads((ROOT / 'package.json').read_text())['version'])
    parser.add_argument('--notes-en', required=True)
    parser.add_argument('--notes-zh', required=True)
    parser.add_argument('--node', default='node')
    args = parser.parse_args()
    try:
        numbers = version_numbers(args.version)
        raw, compressed, count = package(args.sd_zip, args.version)
        digest = hashlib.sha256(compressed).digest()
        body = bytearray(448)
        struct.pack_into('<4s8I', body, 0, b'LNU1', 1, *numbers, 1, len(compressed), len(raw), count)
        body[36:68] = digest
        body[68:228], body[228:388] = note(args.notes_en), note(args.notes_zh)
        manifest = bytes(body) + sign(bytes(body), args.key, args.node)
        target = args.output_dir / (digest.hex() + '.lup')
        if target.exists():
            if target.is_symlink() or target.read_bytes() != compressed:
                raise ValueError('Existing hash-addressed update differs')
        else:
            atomic_write(target, compressed)
        atomic_write(args.output_dir / 'current.manifest', manifest)
        print(json.dumps({'version': args.version, 'files': count, 'compressed_bytes': len(compressed),
                          'raw_bytes': len(raw), 'sha256': digest.hex(), 'package': str(target.resolve()),
                          'manifest': str((args.output_dir / 'current.manifest').resolve())}, indent=2))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
