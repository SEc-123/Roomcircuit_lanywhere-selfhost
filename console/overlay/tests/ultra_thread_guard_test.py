#!/usr/bin/env python3
"""Exercise actual libultrahand thread lifecycle guard with injected failures."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='roomcircuit-ultra-threads-') as tmp:
    target = Path(tmp) / 'guard'
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root / 'console/overlay/tests'), '-I' + str(root / 'console/overlay/include'),
        str(root / 'console/overlay/tests/ultra_thread_guard_test.cpp'), '-o', str(target)], check=True)
    subprocess.run([str(target)], check=True)
print('PASS worker start/close failures, transient wait/close retry and idempotent release')
