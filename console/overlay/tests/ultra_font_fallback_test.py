#!/usr/bin/env python3
"""Exercise actual Ultrahand font selection with missing and Chinese fonts."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
header = (root / 'console/libultrahand/libtesla/include/tesla.hpp').read_text()
start = header.index('            static stbtt_fontinfo* selectFontForCharacterUnsafe(')
end = header.index('        public:', start)
function = header[start:end]
harness = r'''
#include <cassert>
#include <cstdint>
using u32 = std::uint32_t;
struct stbtt_fontinfo { unsigned id; };
static stbtt_fontinfo normal{1}, extended{2}, chinese{3}, taiwan{4}, korean{5};
static stbtt_fontinfo *s_stdFont=&normal, *s_extFont=&extended, *s_localFont=nullptr,
    *s_localFontCN=nullptr, *s_localFontTW=nullptr, *s_localFontKO=nullptr;
static bool s_initialized=false, s_hasLocalFont=false;
static int stbtt_FindGlyphIndex(stbtt_fontinfo *font, u32 code) {
    assert(font);
    return (font->id == 2 && code == 1) || (font->id == 1 && code < 128) ||
        (font->id == 3 && code == 0x4e00) || (font->id == 4 && code == 0x4e01) ||
        (font->id == 5 && code == 0xac00);
}
''' + function + r'''
int main() {
    assert(!selectFontForCharacterUnsafe(0x4e00));
    s_initialized = true;
    assert(selectFontForCharacterUnsafe('A') == &normal);
    assert(selectFontForCharacterUnsafe(0x4e00) == &normal);
    s_localFontCN=&chinese; s_localFontTW=&taiwan; s_localFontKO=&korean;
    assert(selectFontForCharacterUnsafe(0x4e00) == &chinese);
    assert(selectFontForCharacterUnsafe(0x4e01) == &taiwan);
    assert(selectFontForCharacterUnsafe(0xac00) == &korean);
    assert(selectFontForCharacterUnsafe(1) == &extended);
    s_localFont=&taiwan; s_hasLocalFont=true;
    assert(selectFontForCharacterUnsafe(0x4e01) == &taiwan);
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-ultra-font-') as tmp:
    source, target = Path(tmp) / 'font.cpp', Path(tmp) / 'font'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(target)], check=True)
    subprocess.run([str(target)], check=True)
print('PASS actual font fallback: English system, Chinese glyphs, optional fonts missing')
