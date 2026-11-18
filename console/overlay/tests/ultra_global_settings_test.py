#!/usr/bin/env python3
"""Check that the overlay uses Ultrahand settings and theme defaults read-only."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
header = root / 'console/libultrahand/libtesla/include/tesla.hpp'
implementation = root / 'console/libultrahand/libtesla/source/tesla.cpp'
makefile = (root / 'console/overlay/Makefile').read_text()
assert '-DROOMCIRCUIT_NO_GLOBAL_THEME_WRITES' in makefile
assert '-DROOMCIRCUIT_NO_DOWNLOAD' in makefile
assert '-DROOMCIRCUIT_OWN_LANGUAGE' in makefile
assert '-DUI_OVERRIDE_PATH' not in makefile
compiler = shlex.split(os.environ.get('CXX', 'c++'))

def preprocess(path):
    text = '\n'.join('' if line.lstrip().startswith('#include ') else line
                     for line in path.read_text().splitlines())
    return subprocess.run(compiler + [
        '-E', '-P', '-x', 'c++', '-D__SWITCH__=1', '-DROOMCIRCUIT_NO_DOWNLOAD=1',
        '-DROOMCIRCUIT_NO_GLOBAL_THEME_WRITES=1', '-DROOMCIRCUIT_OWN_LANGUAGE=1', '-'], input=text,
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
assert 'initializeTheme(' not in settings
assert 'parseLanguage(' not in settings and 'loadTranslationsFromJSON(' not in settings
assert 'reinitializeLangVars();' in settings
assert 'right_alignment' in h and 'useRightAlignment' in h

theme = source[source.index('Color logoColor1;'):source.index('void initializeTheme(const')]
global_vars = (root / 'console/libultrahand/libultra/source/global_vars.cpp').read_text()
theme_symbols = (' ROOT_PATH', ' BASE_CONFIG_PATH', ' THEME_FILENAME', ' THEME_CONFIG_INI_PATH',
                 ' THEME_STR', ' TRUE_STR')
theme_paths = '\n'.join(line.strip().removeprefix('CONSTEXPR_STRING ') for line in global_vars.splitlines()
                        if line.partition('=')[0].rstrip().endswith(theme_symbols))
harness = r'''
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace ult {
''' + theme_paths + r'''
std::map<std::string, std::map<std::string, std::string>> activeTheme;
auto getParsedDataFromIniFile(const std::string &path) {
    assert(path == "sdmc:/config/ultrahand/theme.ini");
    return activeTheme;
}
int stoi(const char *value) { return std::atoi(value); }
}

struct Color { std::string hex; size_t alpha; };
Color RGB888(const char *hex, size_t alpha = 15) { return {hex, alpha}; }
namespace tsl::style::color { const Color ColorHighlight{}; }
struct ThemeDefault { const char *key; const char *value; };
''' + theme + r'''

int main() {
    for (const auto &setting : defaultThemeSettings) {
        if (std::strcmp(getThemeDefault(setting.key), setting.value) != 0) {
            std::fprintf(stderr, "Missing theme default for %s\n", setting.key);
            return 1;
        }
    }
    initializeThemeVars();
    assert(defaultBackgroundColor.hex == "000000");
    assert(defaultBackgroundColor.alpha == 13);

    ult::activeTheme["theme"] = {{"bg_color", "234567"}, {"bg_alpha", "14"},
                                 {"default_overlay_color", "ABCDEF"}};
    initializeThemeVars();
    assert(defaultBackgroundColor.hex == "234567");
    assert(defaultBackgroundColor.alpha == 14);
    assert(defaultOverlayColor.hex == "ABCDEF");
    assert(unfocusedColor.hex == "666666");
    assert(clickColor.hex == "7777CC" && clickColor.alpha == 7);
    assert(notificationTimeColor.hex == "AAAAAA");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-ultra-theme-') as tmp:
    fixture, target = Path(tmp) / 'theme.cpp', Path(tmp) / 'theme'
    fixture.write_text(harness)
    subprocess.run(compiler + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                   str(fixture), '-o', str(target)], check=True)
    subprocess.run([str(target)], check=True)
print('PASS Ultrahand settings remain read-only and active themes inherit missing defaults')
