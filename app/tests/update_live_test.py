#!/usr/bin/env python3
"""Download through an explicitly supplied update server and install in a disposable host filesystem."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('host')
parser.add_argument('--port', type=int, default=11460)
args = parser.parse_args()
assert 1 <= args.port <= 65535
root = Path(__file__).resolve().parents[2]
harness = r'''
#include "update_core.h"
#include "update_network.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    assert(argc == 5);
    atomic_bool cancel = false;
    unsigned char raw[UPDATE_MANIFEST_SIZE];
    assert(update_network_manifest(argv[1], (unsigned)atoi(argv[2]), raw, &cancel) == UpdateNetworkOk);
    UpdateManifest manifest;
    assert(update_manifest_parse(raw, "0.0.0", &manifest) == UPDATE_OK);
    assert(update_network_download(argv[1], (unsigned)atoi(argv[2]), manifest.bundle_sha256,
        manifest.compressed_size, argv[4], &cancel, NULL, NULL) == UpdateNetworkOk);
    assert(update_install(argv[3], &manifest, argv[4], NULL, NULL) == UPDATE_INSTALLED);
    assert(update_recover(argv[3]) == UPDATE_INSTALLED);
    assert(update_acknowledge(argv[3]) == UPDATE_OK);
    assert(update_recover(argv[3]) == UPDATE_OK);
    printf("PASS public download, production signature, %u managed files, recovery and preserved settings for %s\n",
        UPDATE_FILE_COUNT, manifest.version);
}
'''
with tempfile.TemporaryDirectory(prefix='lanywhere-update-live-') as temporary:
    directory = Path(temporary)
    sd = directory / 'sd'
    (sd / 'lanywhere').mkdir(parents=True)
    setting = sd / 'lanywhere/config.ini'
    setting.write_text('nickname=preserved\n')
    source, executable = directory / 'check.c', directory / 'check'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'app/source'),
        str(source), str(root / 'app/source/update_core.c'), str(root / 'app/source/update_network.c'),
        '-lsodium', '-lz', '-o', str(executable)], check=True)
    subprocess.run([str(executable), args.host, str(args.port), str(sd), str(directory / 'download')], check=True, timeout=600)
    assert setting.read_text() == 'nickname=preserved\n'
