#!/usr/bin/env python3
"""Check the compiled client paths do not rewrite Ultrahand global settings."""
import os
import shlex
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[3]
header = root / 'console/libultrahand/libtesla/include/tesla.hpp'
implementation = root / 'console/libultrahand/libtesla/source/tesla.cpp'
makefile = (root / 'console/overlay/Makefile').read_text()
assert '-DROOMCIRCUIT_NO_GLOBAL_THEME_WRITES' in makefile
assert '-DROOMCIRCUIT_NO_DOWNLOAD' in makefile
assert '-DROOMCIRCUIT_OWN_LANGUAGE' in makefile

def preprocess(path):
    text = '\n'.join('' if line.lstrip().startswith('#include ') else line
                     for line in path.read_text().splitlines())
    return subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-E', '-P', '-x', 'c++', '-D__SWITCH__=1', '-DROOMCIRCUIT_NO_DOWNLOAD=1',
        '-DROOMCIRCUIT_NO_GLOBAL_THEME_WRITES=1', '-DROOMCIRCUIT_OWN_LANGUAGE=1',
        '-DUI_OVERRIDE_PATH="/config/RoomCircuit/"', '-'], input=text,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout

h = preprocess(header)
source = preprocess(implementation)
event = h[h.index('static void backgroundEventPoller('):h.index('static void backgroundSleepPoller(')]
loop = h[h.index('static inline int loop(int argc, char** argv)'):]
settings = source[source.index('void parseOverlaySettings() {'):source.index('int maxNotifications =')]
for name, code in [('event poller', event), ('overlay loop', loop), ('settings', settings)]:
    for forbidden in ('setIniFileValue(', 'saveIniFileData(', 'deleteFileOrDirectory(', 'HOME_EVENT.flag'):
        assert forbidden not in code, f'{name} unexpectedly writes global settings: {forbidden}'
assert 'initializeThemeVars();' in settings and 'sound_effects' in settings
assert 'parseLanguage(' not in settings and 'loadTranslationsFromJSON(' not in settings
assert 'reinitializeLangVars();' in settings
assert 'right_alignment' in h and 'useRightAlignment' in h
print('PASS preprocessed startup/input paths read global theme, sound and alignment without global writes')
