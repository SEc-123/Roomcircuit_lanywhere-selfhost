#!/usr/bin/env python3
"""Exercise actual storage-health IPC fallback and player-facing warning priority."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('routes',root/'app/tests/input_routing_test.py')
routes=importlib.util.module_from_spec(spec);spec.loader.exec_module(routes)
source=(root/'app/source/main.c').read_text()
functions='\n'.join(routes.function(source,name) for name in ('static u32 read_storage_health(', 'static const char *storage_health_text('))
harness=r'''
#include "roomcircuit_paths.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;typedef int Result;typedef int Service;
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define MAKERESULT(m,d) ((m)|((d)<<9))
static unsigned generation;static Result result;static u32 reply;
static unsigned roomcircuit_storage_generation(void){return generation;}
static Result dispatch(Service *s,u32 c,u32 *out){(void)s;assert(c==RC_STORAGE_HEALTH_IPC);*out=reply;return result;}
#define serviceDispatchOut(s,c,o) dispatch((s),(c),&(o))
static const char *ui_text(const char *text){return text;}
''' + functions + r'''
int main(void){Service service=0;
 generation=1;result=MAKERESULT(10,221);assert(read_storage_health(&service)==0);
 generation=3;assert(read_storage_health(&service)==RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED);
 generation=1;result=0;reply=RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT;
 assert(read_storage_health(&service)==reply&&!strcmp(storage_health_text(reply),"Server settings conflict. Compare config.ini files."));
 reply=RC_STORAGE_HEALTH_LEGACY_CHANGED;assert(!strcmp(storage_health_text(read_storage_health(&service)),"Older settings changed. Back up and compare."));
 reply=16;assert(read_storage_health(&service)==RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED);
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-storage-health-') as directory:
    path=Path(directory)/'test.c';binary=Path(directory)/'test';path.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
                   '-I'+str(root/'common'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS storage health: old backend compatibility, conflicts, invalid and warning priority')
