#!/usr/bin/env python3
"""Probe the room snapshot pin, even when the directory's current pin differs."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routing', root / 'app/tests/input_routing_test.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
source = (root / 'app/source/nodes_ui.c').read_text()
worker = mod.function(source, 'static void probe_worker(')
finish = mod.function(source, 'static void finish_room_probe(')
can_retry = mod.function(source, 'static bool room_probe_can_retry(')
code = r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include "node_probe.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RELAY_PROFILE_IPV4 "127.0.0.1"
#define RELAY_PROFILE_COMMUNITY_IPV4 "127.0.0.1"
#define RELAY_PROFILE_PORT 1
#define RELAY_PROFILE_COMMUNITY_PORT 2
typedef struct {NodeMeasurement measurements[NODE_TOTAL_CAPACITY];uint32_t enabled,available;bool catalog_ok,changed;} NodeView;
static NodeView view;
static int view_lock;
static atomic_bool cancel_probe,probe_done[3];
static NodeProbeGuard resolver_guards[3];
static unsigned next_probe;
static uint32_t probe_mask,room_probe_node;
static bool room_probe_pin_valid;
static uint8_t room_probe_pin[4];
static char measured[128];
static void mutexLock(int *p){(void)p;} static void mutexUnlock(int *p){(void)p;}
static bool fetch_catalog(uint32_t *a,uint32_t *b){*a=*b=0;return true;}
static uint32_t node_id(uint32_t id){return id;}
static bool node_copy(uint32_t id,NodeDynamicEntry *out){
 assert(id==13);*out=(NodeDynamicEntry){.id=13,.protocol=NodeProtocolLanPlay,.port=11451,.ipv4={8,8,8,9}};return true;
}
bool node_probe(const char *host,uint16_t port,NodeProbeKind kind,uint32_t id,const atomic_bool *cancel,NodeProbeGuard *guard,NodeMeasurement *out){
 (void)cancel;(void)guard;assert(port==11451&&kind==NodeProbeUdp&&id==13);snprintf(measured,sizeof(measured),"%s",host);
 *out=(NodeMeasurement){.measured_at_ms=42,.state=NodeMeasureReady};return true;
}
static void cancel_probes(void){}
''' + worker + '\n' + finish + '\n' + can_retry + r'''
int main(void){
 NodeMeasurement result={.state=NodeMeasureFailed};
 assert(room_probe_can_retry(&result,false));
 result.state=NodeMeasureReady;assert(!room_probe_can_retry(&result,false));
 result.state=NodeMeasurePending;assert(!room_probe_can_retry(&result,false));
 assert(room_probe_can_retry(&result,true));
 next_probe=13;probe_mask=1u<<13;room_probe_node=13;room_probe_pin_valid=true;
 room_probe_pin[0]=8;room_probe_pin[1]=8;room_probe_pin[2]=8;room_probe_pin[3]=8;
 probe_worker((void*)0);assert(!strcmp(measured,"8.8.8.8"));
 /* A retry for this room must retain its original pin, even if the directory changed. */
 next_probe=13;probe_mask=1u<<13;probe_worker((void*)0);assert(!strcmp(measured,"8.8.8.8"));
 assert(view.measurements[13].measured_at_ms==42);finish_room_probe(13);
 assert(!room_probe_pin_valid&&!view.measurements[13].measured_at_ms&&view.measurements[13].state==NodeMeasurePending);
 next_probe=13;probe_mask=1u<<13;probe_worker((void*)0);assert(!strcmp(measured,"8.8.8.9"));
 puts("PASS room snapshot pin measurement and no stale reuse in hosting picker");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-room-probe-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'common'), '-I'+str(root/'app/source'),
        str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
