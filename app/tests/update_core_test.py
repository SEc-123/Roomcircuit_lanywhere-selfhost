#!/usr/bin/env python3
"""Compile and exercise the updater against a signed fixture and Switch-style rename semantics."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
cc = shlex.split(os.environ.get('CC', 'cc'))
with tempfile.TemporaryDirectory(prefix='lanywhere-update-core-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(cc + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(root / 'app/tests/update_core_test.c'),
        '-lsodium', '-lz', '-o', str(binary)], check=True)
    subprocess.run([str(binary), directory], check=True, timeout=55)

    shim = Path(directory) / 'switch.h'
    shim.write_text('''
#pragma once
#include <stddef.h>
typedef struct { int unused; } Mutex;
void mutexLock(Mutex *);
void mutexUnlock(Mutex *);
void randomGet(void *, size_t);
int fsdevCommitDevice(const char *);
#define R_SUCCEEDED(result) ((result) == 0)
''')
    entropy_test = Path(directory) / 'switch-entropy'
    subprocess.run(cc + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-D__SWITCH__', '-pthread', '-I', directory,
        str(root / 'app/tests/update_switch_entropy_test.c'), '-lsodium', '-lz', '-o', str(entropy_test),
    ], check=True)
    subprocess.run([str(entropy_test)], check=True, timeout=10)
