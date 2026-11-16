#!/usr/bin/env python3
"""Run the actual Ultrahand glyph-cache budget algorithm with fake glyphs."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
header = (root / 'console/libultrahand/libtesla/include/tesla.hpp').read_text()
start = header.index('            static size_t cachedBitmapBytes()')
end = header.index('            // Single helper to clear', start)
functions = header[start:end]
test = r'''
#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
using u64 = std::uint64_t;
struct Glyph {
    void *glyphBmp; int width, height; size_t bytes;
    inline static std::atomic<size_t> liveBitmapBytes{0};
    Glyph(void *bitmap, int w, int h):glyphBmp(bitmap),width(w),height(h),
        bytes(bitmap && w > 0 && h > 0 ? static_cast<size_t>(w)*static_cast<size_t>(h) : 0) {liveBitmapBytes.fetch_add(bytes);}
    ~Glyph() {liveBitmapBytes.fetch_sub(bytes);}
};
static constexpr size_t ROOMCIRCUIT_GLYPH_BITMAP_BUDGET = 256 * 1024;
static std::unordered_map<u64, std::shared_ptr<Glyph>> s_sharedGlyphCache, s_notificationGlyphCache;
''' + functions + r'''
int main() {
    for (u64 i = 0; i < 500; ++i) {
        trimBitmapBudget(1024);
        auto glyph = std::make_shared<Glyph>(reinterpret_cast<void*>(1), 32, 32);
        (i % 2 ? s_sharedGlyphCache : s_notificationGlyphCache)[i] = glyph;
        assert(cachedBitmapBytes() <= ROOMCIRCUIT_GLYPH_BITMAP_BUDGET);
    }
    trimBitmapBudget(ROOMCIRCUIT_GLYPH_BITMAP_BUDGET);
    assert(cachedBitmapBytes() == 0);
    auto pinned = std::make_shared<Glyph>(reinterpret_cast<void*>(1), 32, 32);
    s_sharedGlyphCache[1] = pinned;
    trimBitmapBudget(ROOMCIRCUIT_GLYPH_BITMAP_BUDGET);
    assert(s_sharedGlyphCache.empty() && Glyph::liveBitmapBytes.load() == 1024);
    pinned.reset(); assert(Glyph::liveBitmapBytes.load() == 0);
    s_sharedGlyphCache[1] = std::make_shared<Glyph>(nullptr, 1000, 1000);
    s_sharedGlyphCache[2] = std::make_shared<Glyph>(reinterpret_cast<void*>(1), -1, 10);
    assert(cachedBitmapBytes() == 0);
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-ultra-glyph-') as tmp:
    source = Path(tmp) / 'glyph.cpp'
    target = Path(tmp) / 'glyph'
    source.write_text(test)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(target)], check=True)
    subprocess.run([str(target)], check=True)
print('PASS actual glyph-cache budget across regular and notification caches')
