#!/usr/bin/env python3
"""Exercise the real footer cache when RoomCircuit changes language."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
header = (root / 'console/libultrahand/libtesla/include/tesla.hpp').read_text()
start = header.index('                    static std::string cachedOk, okOverdraw;')
end = header.index('                    renderer->drawStringWithColoredSections(okOverdraw', start)
snippet = header[start:end]
test = r'''
#include <cassert>
#include <string>
namespace ult { std::string GAP_1=" ", GAP_2=" ", OK="OK"; }
static std::string footer() {
''' + snippet + r'''
    return okOverdraw;
}
int main() {
    assert(footer().find("OK") != std::string::npos);
    ult::OK="确定";
    assert(footer().find("确定") != std::string::npos);
    assert(footer().find("OK") == std::string::npos);
    ult::OK="OK";
    assert(footer().find("OK") != std::string::npos);
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-ultra-footer-') as tmp:
    source, target = Path(tmp) / 'footer.cpp', Path(tmp) / 'footer'
    source.write_text(test)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(target)], check=True)
    subprocess.run([str(target)], check=True)
print('PASS actual footer cache switches English, Chinese and back')
