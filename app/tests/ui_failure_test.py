"""Actual diagnostic-capture control path preserves failed requests and existing files."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routing', root/'app/tests/input_routing_test.py')
routing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routing)
source = (root/'app/source/main.c').read_text()
capture = routing.function(source, 'static void save_capture_if_requested(')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#define CAPTURE_FLAG "sdmc:/lanywhere/captura.flag"
static bool g_capture_pending, png_ok;
static int occupied, stat_error, saved, removed;
static int sample_stat(const char *path,struct stat *out) {
    (void)out;
    int number=0;
    assert(sscanf(path,"sdmc:/lanywhere/captura-%d.png",&number)==1);
    if(stat_error){errno=EIO;return -1;}
    if(number<=occupied)return 0;
    errno=ENOENT;return -1;
}
static bool gfx_save_png(const char *path) {
    int number=0;assert(sscanf(path,"sdmc:/lanywhere/captura-%d.png",&number)==1);
    assert(number==occupied+1);saved++;return png_ok;
}
static int sample_remove(const char *path) { assert(!strcmp(path,CAPTURE_FLAG));removed++;return 0; }
#define stat sample_stat
#define remove sample_remove
FUNCTION
#undef stat
#undef remove
int main(void) {
    occupied=999;g_capture_pending=true;save_capture_if_requested();assert(!saved&&!removed);
    occupied=7;g_capture_pending=true;save_capture_if_requested();assert(saved==1&&!removed);
    png_ok=true;g_capture_pending=true;save_capture_if_requested();assert(saved==2&&removed==1);
    stat_error=1;g_capture_pending=true;save_capture_if_requested();assert(saved==2&&removed==1);
    puts("Capture request: full directory, SD failure, failed write and retry passed");
}
'''.replace('FUNCTION', capture).replace('struct sample_stat st;', 'struct stat st;')
# Macro replacement also names the struct token in the actual declaration.
code = code.replace('#define stat sample_stat', '#define stat(path,info) sample_stat(path,info)')
with tempfile.TemporaryDirectory(prefix='capture-error-') as d:
    p=Path(d);(p/'test.c').write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
