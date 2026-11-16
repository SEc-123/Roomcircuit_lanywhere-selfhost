#!/usr/bin/env python3
"""Run the real two-path node configuration reconciler with SD faults."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
fixture = (root / 'console/ldn_mitm/tests/storage_migration_test.py').read_text()
start = fixture.index("harness = r'''\n") + len("harness = r'''")
prefix = fixture[start:fixture.index('/* SOURCE */', start)]
prefix = prefix.replace('#include "storage_migration.hpp"', '#include "node_config_sync.hpp"\n#include "game_identity_sha256.h"\n#include <memory>')
prefix = prefix.replace('using u8 = uint8_t;', 'using u64 = uint64_t; using u8 = uint8_t;')
prefix = prefix.replace('static const int None=0;', 'static const int None=0, Flush=1;')
prefix = prefix.replace('if(!file)return 1;h->handle=file;', 'if(!file)return errno==ENOENT?2:1;h->handle=file;')
base_rename = 'Result RenameFile(const char *a,const char *b){if(fail())return 1;std::error_code e;if(f::exists(path(b)))return 1;f::rename(path(a),path(b),e);return e?1:0;}\n'
assert prefix.count(base_rename) == 1
prefix = prefix.replace(base_rename, '')
prefix += r'''
static bool race_on_rename=false;
namespace ams::fs {
Result RenameFile(const char *a,const char *b){if(fail())return 1;if(f::exists(path(b)))return 1;if(race_on_rename&&!strcmp(a,"sdmc:/config/RoomCircuit/nodes.ini")){race_on_rename=false;std::ofstream(path(a),std::ios::binary|std::ios::trunc)<<"raced-new";}std::error_code e;f::rename(path(a),path(b),e);return e?1:0;}
}
namespace ams::mitm::storage { unsigned generation=3; unsigned StorageGeneration(){return generation;} }
static bool fail_alloc=false; static void *test_calloc(size_t n,size_t size){return fail_alloc?nullptr:std::calloc(n,size);}
'''
source = (root / 'console/ldn_mitm/source/node_config_sync.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
source = source.replace('std::calloc', 'test_calloc')
test = r'''
static void put(const std::string &name,const std::string &value){f::create_directories(f::path(sd+'/'+name).parent_path());std::ofstream(sd+'/'+name,std::ios::binary|std::ios::trunc)<<value;}
static std::string read(const std::string &name){std::ifstream in(sd+'/'+name,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
static void reset(){f::remove_all(sd);f::create_directories(sd+"/config/RoomCircuit");calls=0;fail_at=0;race_on_rename=false;ams::mitm::storage::generation=3;}
static bool get(std::string *value){char out[8192];size_t size=0;bool ok=ams::mitm::storage::ReadNodeConfig(out,sizeof(out),&size);if(ok)*value=std::string(out,size);return ok;}
int main(int argc,char **argv){assert(argc==2);sd=argv[1];std::string value;
 reset();assert(get(&value)&&value.empty());
 reset();put("switch/RoomCircuit/nodes.ini","old");assert(get(&value)&&value=="old");assert(read("config/RoomCircuit/nodes.ini")=="old");
 put("switch/RoomCircuit/nodes.ini","old-script-update");assert(get(&value)&&value=="old-script-update");assert(read("config/RoomCircuit/nodes.ini")=="old-script-update");
 f::remove_all(sd+"/switch/RoomCircuit");assert(get(&value)&&value=="old-script-update");
 reset();put("switch/RoomCircuit/nodes.ini","same");put("config/RoomCircuit/nodes.ini","same");assert(get(&value));
 put("config/RoomCircuit/nodes.ini","new-user-edit");assert(get(&value)&&value=="new-user-edit");assert(read("switch/RoomCircuit/nodes.ini")=="same");assert(get(&value)&&value=="new-user-edit");
 put("switch/RoomCircuit/nodes.ini","stale-script-edit");assert(!get(&value));assert(read("config/RoomCircuit/nodes.ini")=="new-user-edit");
 put("switch/RoomCircuit/nodes.ini","old-newer");put("config/RoomCircuit/nodes.ini","new-newer");assert(!get(&value));assert(read("switch/RoomCircuit/nodes.ini")=="old-newer"&&read("config/RoomCircuit/nodes.ini")=="new-newer");
 f::remove(sd+"/switch/RoomCircuit/nodes.ini");assert(get(&value)&&value=="new-newer");
 reset();put("switch/RoomCircuit/nodes.ini","x");put("config/RoomCircuit/nodes.ini","y");assert(!get(&value));assert(read("switch/RoomCircuit/nodes.ini")=="x"&&read("config/RoomCircuit/nodes.ini")=="y");
 reset();put("switch/RoomCircuit/nodes.ini","keep");fail_alloc=true;assert(!get(&value));fail_alloc=false;assert(read("switch/RoomCircuit/nodes.ini")=="keep"&&get(&value)&&value=="keep");
 reset();put("switch/RoomCircuit/nodes.ini","a");assert(get(&value));put("switch/RoomCircuit/nodes.ini","b");race_on_rename=true;assert(!get(&value));assert(read("config/RoomCircuit/nodes.ini")=="raced-new"&&read("switch/RoomCircuit/nodes.ini")=="b");assert(!get(&value));
 reset();put("switch/RoomCircuit/nodes.ini","first");calls=0;fail_at=999999;assert(get(&value));const int first_steps=calls;
 for(int i=1;i<=first_steps;i++){
  reset();put("switch/RoomCircuit/nodes.ini","first");calls=0;fail_at=i;(void)get(&value);fail_at=0;calls=0;
  assert(read("switch/RoomCircuit/nodes.ini")=="first");assert(get(&value)&&value=="first"&&read("config/RoomCircuit/nodes.ini")=="first");
 }
 reset();put("switch/RoomCircuit/nodes.ini","a");assert(get(&value));put("switch/RoomCircuit/nodes.ini","b");calls=0;fail_at=999999;assert(get(&value));const int steps=calls;
 for(int i=1;i<=steps;i++){
  reset();put("switch/RoomCircuit/nodes.ini","a");assert(get(&value));put("switch/RoomCircuit/nodes.ini","b");calls=0;fail_at=i;
  (void)get(&value);fail_at=0;calls=0;
  assert(read("switch/RoomCircuit/nodes.ini")=="b");
  assert(get(&value));assert(value=="b"&&read("config/RoomCircuit/nodes.ini")=="b");
 }
 reset();f::create_directories(sd+"/config/RoomCircuit/.nodes-sync");put("config/RoomCircuit/.nodes-sync/foreign","keep");assert(!get(&value));assert(read("config/RoomCircuit/.nodes-sync/foreign")=="keep");
 reset();ams::mitm::storage::generation=1;put("switch/RoomCircuit/nodes.ini","v1");put("config/RoomCircuit/nodes.ini","v2");assert(get(&value)&&value=="v1");
 puts("PASS real nodes.ini reconciliation, both edit directions, conflicts, old folder deletion, and every SD failure");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-node-config-') as temp:
    p=Path(temp); cpp=p/'test.cpp'; exe=p/'test'; cpp.write_text(prefix+source+test)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++20','-Wall','-Wextra','-Werror',
        '-I'+str(root/'common'),'-I'+str(root/'console/ldn_mitm/source'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe),str(p/'sd')],check=True,timeout=60)
