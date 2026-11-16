#!/usr/bin/env python3
"""Check the real main.c storage selector at actual service/IPC boundaries."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routing', root / 'app/tests/input_routing_test.py')
routing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routing)
selector = routing.function((root / 'app/source/main.c').read_text(), 'static void select_module_storage(')
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int Result; typedef int Service; typedef int SmServiceName;
#define R_FAILED(x) ((x) != 0)
#define R_SUCCEEDED(x) ((x) == 0)
static int tipc_result, presence, get_result, dispatch_result, response, opens, closes, calls;
static bool configured; static unsigned generation;
static SmServiceName smEncodeName(const char *name){assert(!strcmp(name,"lanywhr"));return 7;}
static int smGetServiceSessionTipc(void){return 8;}
static int fake_tipc(int session,int command,int name,u8 *out){assert(session==8&&command==65100&&name==7);*out=presence;return tipc_result;}
#define tipcDispatchInOut(s,c,n,o) fake_tipc(s,c,n,&(o))
static int smGetService(Service *s,const char *name){assert(!strcmp(name,"lanywhr"));opens++;*s=9;return get_result;}
static int fake_dispatch(Service *s,int command,u32 *out){assert(*s==9&&command==46);calls++;*out=response;return dispatch_result;}
#define serviceDispatchOut(s,c,o) fake_dispatch(s,c,&(o))
static void serviceClose(Service *s){assert(*s==9);closes++;}
static void roomcircuit_storage_set_generation(unsigned value){assert(!configured);configured=true;generation=value;}
/* SELECTOR */
static void reset(void){tipc_result=presence=get_result=dispatch_result=response=opens=closes=calls=0;configured=false;generation=99;}
int main(void){
 reset();select_module_storage();assert(!configured&&!opens&&!calls&&!closes);
 reset();tipc_result=1;presence=1;select_module_storage();assert(!configured&&!opens);
 reset();presence=1;get_result=1;select_module_storage();assert(!configured&&opens==1&&!closes);
 reset();presence=1;dispatch_result=1;select_module_storage();assert(!configured&&calls==1&&closes==1);
 reset();presence=1;response=2;select_module_storage();assert(!configured&&closes==1);
 reset();presence=1;response=4;select_module_storage();assert(!configured&&closes==1);
 reset();presence=1;response=0;select_module_storage();assert(configured&&generation==0&&closes==1);
 reset();presence=1;response=1;select_module_storage();assert(configured&&generation==1&&closes==1);
 reset();presence=1;response=3;select_module_storage();assert(configured&&generation==3&&closes==1);
 puts("PASS actual storage IPC selector: absent service, failure, unsupported IPC, invalid selection, legacy and current");
}
'''.replace('/* SELECTOR */', selector)
with tempfile.TemporaryDirectory(prefix='roomcircuit-storage-ipc-') as directory:
    folder=Path(directory); test=folder/'test.c'; binary=folder/'test';test.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-std=c11','-Wall','-Wextra','-Werror',str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
