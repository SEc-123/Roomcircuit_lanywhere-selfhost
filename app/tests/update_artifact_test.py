#!/usr/bin/env python3
"""Install a real signed update into a disposable filesystem using the production verification key."""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('manifest', type=Path)
parser.add_argument('bundle', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
harness = r'''
#include "update_core.h"
#include <assert.h>
#include <stdio.h>
int main(int argc, char **argv) {
    assert(argc == 4);
    unsigned char raw[UPDATE_MANIFEST_SIZE];
    FILE *file = fopen(argv[2], "rb"); assert(file);
    assert(fread(raw, 1, sizeof(raw), file) == sizeof(raw) && fgetc(file) == EOF);
    assert(fclose(file) == 0);
    UpdateManifest manifest;
    assert(update_manifest_parse(raw, "0.0.0", &manifest) == UPDATE_OK);
    assert(update_install(argv[1], &manifest, argv[3], NULL, NULL) == UPDATE_INSTALLED);
    assert(update_recover(argv[1]) == UPDATE_INSTALLED);
    char version[32]; assert(update_pending_version(argv[1], version) == UPDATE_INSTALLED);
    assert(update_acknowledge(argv[1]) == UPDATE_OK);
    assert(update_recover(argv[1]) == UPDATE_OK);
    printf("Production-key signature, exact bundle, %u managed files and commit verified for %s\n", UPDATE_FILE_COUNT, version);
}
'''
with tempfile.TemporaryDirectory(prefix='lanywhere-real-update-') as temporary:
    directory = Path(temporary)
    sd = directory / 'sd'
    (sd / 'lanywhere').mkdir(parents=True)
    setting = sd / 'lanywhere/config.ini'
    setting.write_text('nickname=preserved\n')
    (directory / 'check.c').write_text(harness)
    executable = directory / 'check'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'app/source'),
        str(directory / 'check.c'), str(root / 'app/source/update_core.c'),
        '-lsodium', '-lz', '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(sd), str(args.manifest.resolve()), str(args.bundle.resolve())], check=True)
    assert setting.read_text() == 'nickname=preserved\n'
