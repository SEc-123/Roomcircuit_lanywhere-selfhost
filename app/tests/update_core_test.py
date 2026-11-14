#!/usr/bin/env python3
"""Compile and exercise the updater against a signed fixture and Switch-style rename semantics."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='lanywhere-update-core-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(root / 'app/tests/update_core_test.c'),
        '-lsodium', '-lz', '-o', str(binary)], check=True)
    subprocess.run([str(binary), directory], check=True, timeout=55)
