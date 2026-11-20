#!/usr/bin/env python3
"""Exercise actual app IPC42 handling for recoverable custom-node errors."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('extract',root/'app/tests/input_routing_test.py')
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
source=(root/'app/source/nodes_ui.c').read_text()
slot=extract.function(source,'static uint32_t node_slot(')
refresh=extract.function(source,'bool nodes_ui_refresh_catalog(')
harness=r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int Result;typedef int Service;
#define R_FAILED(x) ((x)!=0)
typedef struct {bool changed, local_config_invalid;} NodeView;
static NodeView view;static int view_lock;static unsigned dynamic_generation;
static NodeDynamicEntry dynamic_nodes[NODE_TOTAL_CAPACITY];static bool dynamic_present[NODE_TOTAL_CAPACITY];
static char dynamic_regions[NODE_DYNAMIC_CAPACITY][NODE_REGION_LABEL_BYTES];
static unsigned dynamic_enabled,dynamic_available;static NodeDynamicCatalog supplied;
static void mutexLock(int*x){(void)x;}static void mutexUnlock(int*x){(void)x;}
static int dispatch(Service*s,unsigned command,NodeDynamicCatalog*out){(void)s;assert(command==NODE_GET_CATALOG_IPC);*out=supplied;return 0;}
#define serviceDispatch(s,c,...) dispatch((s),(c),&catalog)
''' + slot + '\n' + refresh + r'''
int main(void){Service service=0;supplied.version=1;supplied.count=1;
 supplied.nodes[0].id=13;supplied.nodes[0].protocol=NodeProtocolLanPlay;supplied.nodes[0].port=11451;
 supplied.nodes[0].flags=1;strcpy(supplied.nodes[0].name,"Shared");strcpy(supplied.nodes[0].host,"example.org");
 supplied.enabled=NODE_LOCAL_CONFIG_INVALID;assert(nodes_ui_refresh_catalog(&service));
 assert(view.local_config_invalid&&view.changed&&dynamic_present[13]&&!dynamic_present[24]);
 unsigned generation=dynamic_generation;view.changed=false;supplied.enabled=0;
 assert(nodes_ui_refresh_catalog(&service));assert(!view.local_config_invalid&&view.changed&&dynamic_generation>generation&&dynamic_present[13]);
 puts("PASS actual app catalog marks custom configuration error and recovers while retaining shared nodes");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-node-ui-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(harness)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'common'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
