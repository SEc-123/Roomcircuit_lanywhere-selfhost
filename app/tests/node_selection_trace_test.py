#!/usr/bin/env python3
"""Exercise the actual bounded selection recorder through failed SD operations."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_mode, opened, closed;
static unsigned flushed, synced;
static int checked_mkdir(const char *path, mode_t mode) {
    if (fail_mode == 1) { errno = EIO; return -1; }
    return mkdir(path, mode);
}
static FILE *checked_fopen(const char *path, const char *mode) {
    if (fail_mode == 2) { errno = EIO; return NULL; }
    FILE *file = fopen(path, mode);
    if (file) opened++;
    return file;
}
static size_t checked_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    if (fail_mode == 3) { errno = EIO; return 0; }
    return fwrite(data, size, count, file);
}
static int checked_fclose(FILE *file) {
    closed++;
    const int result = fclose(file);
    if (fail_mode == 4) { errno = EIO; return EOF; }
    return result;
}
static int checked_fseek(FILE *file, long offset, int whence) {
    if (fail_mode == 5) { errno = EIO; return -1; }
    return fseek(file, offset, whence);
}
static long checked_ftell(FILE *file) {
    if (fail_mode == 6) { errno = EIO; return -1; }
    return ftell(file);
}
static int checked_fflush(FILE *file) {
    if (fail_mode == 7) { errno = EIO; return EOF; }
    flushed++;
    return fflush(file);
}
static int checked_fileno(FILE *file) {
    if (fail_mode == 8) { errno = EBADF; return -1; }
    return fileno(file);
}
static int checked_fsync(int descriptor) {
    if (fail_mode == 9) { errno = EIO; return -1; }
    synced++;
    return fsync(descriptor);
}
#define mkdir checked_mkdir
#define fopen checked_fopen
#define fwrite checked_fwrite
#define fclose checked_fclose
#define fseek checked_fseek
#define ftell checked_ftell
#define fflush checked_fflush
#define fileno checked_fileno
#define fsync checked_fsync
#define NODE_SELECTION_TRACE_DIRECTORY "trace"
#define LANYWHERE_APP_VERSION "0.8.8"
#include "node_selection_trace.c"
#undef mkdir
#undef fopen
#undef fwrite
#undef fclose
#undef fseek
#undef ftell
#undef fflush
#undef fileno
#undef fsync

static char saved[4096];
static size_t read_record(void) {
    FILE *file = fopen("trace/node-selection-app.log", "rb");
    assert(file);
    const size_t size = fread(saved, 1, sizeof(saved) - 1, file);
    assert(!ferror(file) && fclose(file) == 0);
    saved[size] = 0;
    return size;
}

int main(void) {
    errno = EDOM;
    node_selection_trace_begin(2);
    node_selection_trace_phase(NodeSelectionBeforeIpc, 0);
    assert(errno == EDOM);
    assert(flushed == 2 && synced == 2);
    read_record();
    assert(strstr(saved, "app=0.8.8 build=node-select-20261116b node=2\n"));
    assert(strstr(saved, "phase=before_ipc result=pending"));
    assert(!strstr(saved, "phase=after_ipc"));
    node_selection_trace_phase(NodeSelectionAfterIpc, 0x1159);
    read_record();
    assert(strstr(saved, "phase=after_ipc result=0x00001159"));
    node_selection_trace_begin(7);
    node_selection_trace_phase(NodeSelectionBeforeCancelProbes, 0);
    node_selection_trace_phase(NodeSelectionAfterCancelProbes, 0);
    read_record();
    assert(strstr(saved, "node=7") && !strstr(saved, "node=2"));
    assert(!strstr(saved, "phase=after_ipc"));
    assert(strstr(saved, "phase=after_cancel_probes result=0x00000000"));

    // Neither invalid phases nor repeated writes can grow an unbounded log.
    const size_t before = read_record();
    node_selection_trace_phase((NodeSelectionTracePhase)99, 0);
    assert(read_record() == before);
    for (unsigned i = 0; i < 1000; i++) node_selection_trace_phase(NodeSelectionAfterIpc, 0);
    assert(read_record() <= TRACE_LIMIT && !trace_ready);
    node_selection_trace_begin(3);
    assert(trace_ready && read_record() < 160);

    for (fail_mode = 1; fail_mode <= 9; fail_mode++) {
        errno = EDOM;
        node_selection_trace_begin(8);
        assert(errno == EDOM && !trace_ready && opened == closed);
        node_selection_trace_phase(NodeSelectionBeforeIpc, 0);
        assert(errno == EDOM && opened == closed);
    }
    fail_mode = 0;
    node_selection_trace_begin(9);
    fail_mode = 2;
    errno = EDOM;
    node_selection_trace_phase(NodeSelectionBeforeIpc, 0);
    assert(errno == EDOM && !trace_ready);
    fail_mode = 0;
    node_selection_trace_begin(10);
    assert(trace_ready);
    node_selection_trace_phase(NodeSelectionBeforeIpc, 0);
    node_selection_trace_phase(NodeSelectionAfterIpc, 0);
    read_record();
    assert(strstr(saved, "node=10") && opened == closed);
    puts("PASS selection trace: flushed and synced pre-IPC record, result, truncation, size bound, nine SD failures and retry");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-node-trace-') as temporary:
    temporary = Path(temporary)
    source, binary = temporary / 'fixture.c', temporary / 'fixture'
    source.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'app/source'),
        str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], cwd=temporary, check=True)
