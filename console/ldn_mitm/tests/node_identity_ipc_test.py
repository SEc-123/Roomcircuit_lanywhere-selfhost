#!/usr/bin/env python3
"""Test the actual host-selection service boundary with malformed IPC buffers."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
extract_path = root / 'console/ldn_mitm/tests/discovery_route_test.py'
spec = importlib.util.spec_from_file_location('extract', extract_path)
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
source = (root / 'console/ldn_mitm/source/lanywhere_service.cpp').read_text()
method = extract.function(source, 'Result LanywhereService::SetHostNodeVerified(')
getter = extract.function(source, 'Result LanywhereService::GetHostNodeIdentity(')
health = extract.function(source, 'Result LanywhereService::GetStorageHealth(')
start = extract.function(source, 'Result LanywhereService::StartHostNodeTask(')
task = extract.function(source, 'Result LanywhereService::GetHostNodeTask(')
harness = r'''
#include "node_lobby.h"
#include <cassert>
#include <cstring>
using Result = int;
#define R_RETURN(x) return (x)
#define R_SUCCEED() return 0
#define R_TRY(x) do { const Result rc=(x); if(rc) return rc; } while(0)
static Result ResultRelayFailed() { return 1; }
namespace sf { struct InBuffer { const void *ptr; size_t size; size_t GetSize()const{return size;} const void *GetPointer()const{return ptr;} }; }
namespace sf { struct OutBuffer { void *ptr; size_t size; size_t GetSize()const{return size;} void *GetPointer()const{return ptr;} }; }
namespace sf { template<class T> struct Out { T *ptr; void SetValue(T value){*ptr=value;} }; }
using u32 = unsigned;
using u64 = uint64_t;
namespace storage { static unsigned StorageHealth(){return 2;} }
namespace lanywhere { static unsigned calls; static NodeDynamicEntry copied; static Result SetHostNodeVerified(const NodeDynamicEntry &v){++calls;copied=v;return 0;} static bool GetHostNodeIdentity(NodeDynamicEntry *out){*out=copied;return true;} static Result StartHostNodeTask(const NodeDynamicEntry &v,u64 *id){copied=v;*id=17;return 0;} static NodeHostTaskStatus GetHostNodeTask(u64 id){NodeHostTaskStatus value{};value.version=1;value.task_id=id;return value;} }
class LanywhereService { public: Result SetHostNodeVerified(const sf::InBuffer &identity); Result GetHostNodeIdentity(const sf::OutBuffer &identity); Result GetStorageHealth(sf::Out<u32> health); Result StartHostNodeTask(const sf::InBuffer &identity,sf::Out<u64> task_id); Result GetHostNodeTask(u64 id,const sf::OutBuffer &out); };
''' + method + '\n' + getter + '\n' + health + '\n' + start + '\n' + task + r'''
int main(){LanywhereService service;NodeDynamicEntry entry{};entry.id=13;entry.port=11451;
 assert(service.SetHostNodeVerified({nullptr,sizeof(entry)})==1 && !lanywhere::calls);
 assert(service.SetHostNodeVerified({&entry,sizeof(entry)-1})==1 && !lanywhere::calls);
 assert(service.SetHostNodeVerified({&entry,sizeof(entry)+1})==1 && !lanywhere::calls);
 assert(service.SetHostNodeVerified({&entry,sizeof(entry)})==0 && lanywhere::calls==1);
 assert(!std::memcmp(&entry,&lanywhere::copied,sizeof(entry)));
 NodeDynamicEntry output{};
 assert(service.GetHostNodeIdentity({nullptr,sizeof(output)})==1);
 assert(service.GetHostNodeIdentity({&output,sizeof(output)-1})==1);
 assert(service.GetHostNodeIdentity({&output,sizeof(output)})==0&&!std::memcmp(&entry,&output,sizeof(entry)));
 u32 state=0;assert(service.GetStorageHealth({&state})==0&&state==2);
 u64 id=0;assert(service.StartHostNodeTask({nullptr,sizeof(entry)},{&id})==1&&id==0);
 assert(service.StartHostNodeTask({&entry,sizeof(entry)},{&id})==0&&id==17);
 NodeHostTaskStatus status{};assert(service.GetHostNodeTask(id,{nullptr,sizeof(status)})==1);
 assert(service.GetHostNodeTask(id,{&status,sizeof(status)})==0&&status.version==1&&status.task_id==id);
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-identity-ipc-') as directory:
    source_file = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    source_file.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++20','-Wall','-Wextra','-Werror',
                   '-I'+str(root/'common'),str(source_file),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS identity selection IPC rejects malformed buffers and forwards exact identity')
