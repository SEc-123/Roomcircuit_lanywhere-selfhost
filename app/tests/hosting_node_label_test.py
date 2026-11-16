#!/usr/bin/env python3
"""Exercise the real host-label formatter with cached and unavailable nodes."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'app/source/main.c').read_text()
start = source.index('static const char *hosting_node_label(')
end = source.index('\nstatic bool consume_inventory_result(', start)
function = source[start:end]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "common/node_lobby.h"
typedef struct { NodeHostInfo host_info; bool host_info_valid; } View;
static const char *ui_text(const char *value) { return value; }
''' + function + r'''
int main(void) {
    View view = {0}; char value[96];
    assert(!strcmp(hosting_node_label(&view, value, sizeof(value)), "Unavailable"));
    view.host_info_valid = true;
    strcpy(view.host_info.name, "Russia LAN");
    view.host_info.status = NodeHostReady;
    assert(!strcmp(hosting_node_label(&view, value, sizeof(value)), "Russia LAN"));
    view.host_info.status = NodeHostUnavailable;
    assert(!strcmp(hosting_node_label(&view, value, sizeof(value)), "Russia LAN · Unavailable"));
    view.host_info.status = NodeHostPending;
    assert(!strcmp(hosting_node_label(&view, value, sizeof(value)), "Russia LAN · Checking"));
    puts("PASS host label preserves saved identity and shows unavailable or pending");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-host-label-') as directory:
    source_file = Path(directory) / 'test.c'
    executable = Path(directory) / 'test'
    source_file.write_text(harness)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root), str(source_file), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
