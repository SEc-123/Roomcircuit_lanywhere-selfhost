#!/usr/bin/env python3
"""Exercise the actual picker readback and current-node identity check."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routes', root / 'app/tests/input_routing_test.py')
routes = importlib.util.module_from_spec(spec); spec.loader.exec_module(routes)
source = (root/'app/source/nodes_ui.c').read_text()
functions = '\n'.join(routes.function(source, name) for name in (
    'static void refresh_host_info(', 'static bool node_matches_current('))
harness = r'''
#include "node_lobby.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int Service;typedef int Result;
#define R_SUCCEEDED(x) ((x)==0)
enum {SfBufferAttr_Out=1,SfBufferAttr_HipcMapAlias=2};
typedef struct {int buffer_attrs[1];struct {void *ptr;size_t size;} buffers[1];} Params;
static NodeHostInfo current_host_info,remote_info;
static bool current_host_valid,current_host_identity_valid;
static NodeDynamicEntry current_host_identity,remote_identity;
static int failed_command;
static Result dispatch(Service *s,int command,Params p){(void)s;if(command==failed_command)return 1;
 if(command==NODE_GET_HOST_INFO_IPC){assert(p.buffers[0].size==sizeof(remote_info));memcpy(p.buffers[0].ptr,&remote_info,sizeof(remote_info));return 0;}
 if(command==NODE_GET_HOST_IDENTITY_IPC){assert(p.buffers[0].size==sizeof(remote_identity));memcpy(p.buffers[0].ptr,&remote_identity,sizeof(remote_identity));return 0;}
 assert(false);return 1;}
#define serviceDispatch(s,c,...) dispatch((s),(c),(Params){__VA_ARGS__})
''' + functions + r'''
int main(void){Service service=0;remote_info.version=1;remote_info.node_id=13;remote_info.status=NodeHostReady;
 strcpy(remote_info.name,"Shared");strcpy(remote_info.host,"host.example");remote_identity.id=13;
 remote_identity.flags=1;remote_identity.port=11451;strcpy(remote_identity.host,"host.example");strcpy(remote_identity.name,"Shared");
 refresh_host_info(&service);assert(node_matches_current(&remote_identity,13));
 failed_command=NODE_GET_HOST_INFO_IPC;refresh_host_info(&service);assert(!current_host_valid&&!node_matches_current(&remote_identity,13));
 failed_command=NODE_GET_HOST_IDENTITY_IPC;refresh_host_info(&service);assert(current_host_valid&&!current_host_identity_valid&&!node_matches_current(&remote_identity,13));
 failed_command=0;refresh_host_info(&service);NodeDynamicEntry changed=remote_identity;changed.ipv4[3]=9;assert(!node_matches_current(&changed,13));
 memset(remote_identity.host,'a',110);remote_identity.host[110]=0;refresh_host_info(&service);
 changed=remote_identity;changed.host[100]='b';assert(!node_matches_current(&changed,13));
 remote_info.status=NodeHostUnavailable;refresh_host_info(&service);assert(!node_matches_current(&remote_identity,13));
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-host-info-') as directory:
    source_file=Path(directory)/'test.c';binary=Path(directory)/'test';source_file.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
                   '-I'+str(root/'common'),str(source_file),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS current node readback: IPC failure clears stale state, full hostname and pin verified')
