#!/usr/bin/env python3
"""Run the actual room-check input loop, including retry and pin preservation."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routing', root / 'app/tests/input_routing_test.py')
routing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routing)
source = (root / 'app/source/nodes_ui.c').read_text()
retry = routing.function(source, 'static bool room_probe_can_retry(')
check = routing.function(source, 'bool nodes_ui_check_room(')
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "node_lobby.h"
#include "node_catalog.h"
#include "node_probe.h"
typedef uint64_t u64;
typedef struct { int unused; } PadState;
typedef struct { int unused; } NodeProbeLoop;
typedef struct { NodeMeasurement measurements[NODE_TOTAL_CAPACITY]; bool probe_start_failed,changed; } NodeView;
enum { HidNpadButton_A=1, HidNpadButton_B=2, HidNpadButton_X=4, HidNpadButton_StickR=8 };
static NodeView view;
static int view_lock, frame, starts, finishes;
static uint32_t room_probe_node;
static bool room_probe_pin_valid;
static uint8_t room_probe_pin[4];
static bool nodes_ui_valid(uint32_t id) { return id == 13; }
static void cancel_probes(void) {}
static void start_probes(uint32_t mask, bool force) {
    assert(mask==(1u<<13) && force && room_probe_pin_valid && room_probe_node==13);
    assert(!memcmp(room_probe_pin, (uint8_t[]){8,8,8,8}, 4));
    ++starts;
    view.measurements[13].state=starts==1 ? NodeMeasureFailed : NodeMeasureReady;
}
static NodeProbeLoop probe_loop_start(void) { return (NodeProbeLoop){0}; }
static void collect_probes(void) {}
static void maintain_probes(NodeProbeLoop *loop,uint32_t mask) { (void)loop;(void)mask; }
static bool appletMainLoop(void) { return frame < 3; }
static void padUpdate(PadState *pad) { (void)pad; }
static u64 padGetButtonsDown(PadState *pad) { (void)pad; const u64 keys[]={HidNpadButton_A,HidNpadButton_X,HidNpadButton_A}; return keys[frame++]; }
static NodeView snapshot(void) { return view; }
static bool modal_touch_start(bool *held,int *x,int *y) { (void)held;(void)x;(void)y;return false; }
static void language_input(u64 down) { (void)down; }
static void mutexLock(int *lock) { (void)lock; }
static void mutexUnlock(int *lock) { (void)lock; }
static void draw_room_check(const NodeMeasurement *m,uint32_t node,bool gateway,bool failed) { (void)m;(void)node;(void)gateway;(void)failed; }
static void svcSleepThread(uint64_t ns) { (void)ns; }
static void finish_room_probe(uint32_t id) { assert(id==13); ++finishes; room_probe_pin_valid=false; }
''' + retry + '\n' + check + r'''
int main(void) {
    PadState pad={0};
    NodeRoomLocator locator={0};
    memcpy(locator.reserved,(uint8_t[]){8,8,8,8},4);
    assert(!nodes_ui_check_room(&pad,13,false,NULL) && starts==0);
    assert(nodes_ui_check_room(&pad,13,false,&locator));
    assert(frame==3 && starts==2 && finishes==1 && !room_probe_pin_valid);
    puts("PASS room probe failed -> X retry -> A continue, retaining exact room pin");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-room-probe-input-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'common'), '-I'+str(root/'app/source'),
        str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
