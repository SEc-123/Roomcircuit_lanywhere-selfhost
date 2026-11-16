#!/usr/bin/env python3
"""Add an Ed25519 ownership record to an official renamed-layout SD ZIP."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('roomcircuit_update_signing', ROOT / 'scripts/make-online-update.py')
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


def proof_bytes(app, overlay, version, key, node):
    update.version_numbers(version)
    encoded = version.encode('ascii')
    if len(encoded) >= 16:
        raise ValueError('Version is too long for the entrypoint ownership record')
    body = bytearray(b'RCI1' + encoded.ljust(16, b'\0'))
    for data in (app, overlay):
        if not data or len(data) > update.MAX_RAW:
            raise ValueError('Invalid entrypoint size')
        update.verify_nro_version(data, version)
        body += struct.pack('<I', len(data)) + hashlib.sha256(data).digest()
    assert len(body) == 92
    proof = bytes(body) + update.sign(bytes(body), key, node)
    update.verify_entrypoint_proof(proof, app, overlay, version, node)
    return proof


def sign_sd(input_path, output_path, version, key, node='node'):
    if input_path.is_symlink() or not input_path.is_file():
        raise ValueError('Input must be a regular SD ZIP')
    if output_path.exists() or output_path.is_symlink():
        raise ValueError('Output already exists')
    if input_path.resolve() == output_path.resolve():
        raise ValueError('Input and output must differ')
    # Validate all 22 legacy updater IDs and both renamed entrypoints first.
    update.package(input_path, version, node)
    with zipfile.ZipFile(input_path) as archive:
        infos = archive.infolist()
        names = {item.filename for item in infos}
        if update.ENTRYPOINT_PROOF in names or update.ENTRYPOINT_APP not in names or update.ENTRYPOINT_OVERLAY not in names:
            raise ValueError('Expected an unsigned renamed-layout SD ZIP')
        proof = proof_bytes(archive.read(update.ENTRYPOINT_APP), archive.read(update.ENTRYPOINT_OVERLAY), version, key, node)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(prefix='.entrypoints-', suffix='.zip', dir=output_path.parent)
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            with zipfile.ZipFile(temporary, 'w') as signed:
                for info in infos:
                    signed.writestr(info, archive.read(info.filename))
                signed.writestr(update.ENTRYPOINT_PROOF, proof, compress_type=zipfile.ZIP_DEFLATED)
            # Recheck the proof and the 22-file OTA mapping before exposing output.
            update.package(temporary, version, node)
            with temporary.open('rb') as stream:
                os.fsync(stream.fileno())
            os.link(temporary, output_path)  # atomic and fails if the destination exists
            directory = os.open(output_path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            temporary.unlink(missing_ok=True)
    return {'version': version, 'sd_zip': str(output_path.resolve()),
            'sha256': hashlib.sha256(output_path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sd-zip', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--key', type=Path, required=True)
    parser.add_argument('--version', default=json.loads((ROOT / 'package.json').read_text())['version'])
    parser.add_argument('--node', default='node')
    args = parser.parse_args()
    try:
        print(json.dumps(sign_sd(args.sd_zip, args.output, args.version, args.key, args.node), indent=2))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
