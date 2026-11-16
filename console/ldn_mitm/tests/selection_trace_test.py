#!/usr/bin/env python3
"""Run actual selection diagnostics and SetHostNode through failed SD operations."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / 'console/ldn_mitm/source'
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('discovery_route_test.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
logger = (NATIVE / 'selection_trace.cpp').read_text()
logger = '\n'.join(line for line in logger.splitlines() if not line.startswith(('#include', '#if', '#endif')))
logger = '#include "'+str(ROOT / 'common/roomcircuit_paths.h')+'"\nnamespace ams::mitm::storage { bool StorageUseCurrent() { return false; } }\n'+logger
relay = (NATIVE / 'lanywhere_relay.cpp').read_text()
select = (extract.function(relay, 'Result SetHostNodeInternal(u32 node, u64 expected_generation) {') + '\n' +
          extract.function(relay, 'Result SetHostNode(u32 node) {'))
fixture = r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using s64=int64_t;using Result=int;
#define GITDESCVER "0.8.8-test"
#define R_FAILED(v) ((v)!=0)
#define R_SUCCEEDED(v) ((v)==0)
#define R_SUCCEED() return 0
static thread_local unsigned char tls[0x100];
static void *armGetTls(){return tls;}
static unsigned calls=0,fail_at=0,opens=0,closes=0,flushes=0;
static bool exists=false;
static std::string file;
static bool fault(){std::memset(tls,0,0x100);errno=EIO;return ++calls==fail_at;}
namespace ams {
namespace os {using SdkMutex=std::mutex;}
namespace util {
int TSNPrintf(char*out,size_t cap,const char*format,...){va_list args;va_start(args,format);int n=vsnprintf(out,cap,format,args);va_end(args);return n;}
}
namespace fs {
using FileHandle=int;constexpr int OpenMode_Write=1,OpenMode_AllowAppend=2;
struct WriteOption{static constexpr int Flush=1;};
int CreateDirectory(const char*){return fault()?1:0;}
int HasFile(bool*out,const char*){if(fault())return 1;*out=exists;return 0;}
int CreateFile(const char*,s64){if(fault())return 1;exists=true;return 0;}
int OpenFile(FileHandle*out,const char*,int){if(fault())return 1;*out=1;++opens;return 0;}
int SetFileSize(FileHandle,s64 n){if(fault())return 1;file.resize(n);return 0;}
void CloseFile(FileHandle){++closes;std::memset(tls,0,0x100);}
int WriteFile(FileHandle,s64 offset,const void*data,size_t n,int options){
 assert(options==WriteOption::Flush);
 if(fault())return 1;
 assert(offset>=0&&size_t(offset)+n<=8192);file.resize(offset+n);
 std::memcpy(file.data()+offset,data,n);++flushes;return 0;
}
}
namespace mitm::selection_trace {
using Handle=uint64_t;
Handle Begin(uint32_t,const char*);
void Record(Handle,const char*,uint32_t=0);
}
}
/* LOGGER */
namespace selection_trace=ams::mitm::selection_trace;

static std::atomic<bool>g_direct_session=false;
static std::atomic<u64>g_host_preference_generation{1};
static u32 g_selected_status=NodeHostSelected;
static NodeDynamicEntry g_selected_node{};
static size_t NodeSlot(u32 id){return id<NODE_CATALOG_COUNT?id:NODE_TOTAL_CAPACITY;}
static bool LookupNode(u32 id,NodeDynamicEntry*out){*out={};out->id=id;out->flags=1;return id<NODE_CATALOG_COUNT;}
static bool SaveNodePreference(const NodeDynamicEntry&){return true;}
static void StartPreferenceRetry(){}
static std::mutex g_lock,g_match_lock,g_room_lifecycle_lock;
enum class RelayState {Idle,InRoom,Connecting};
static RelayState g_state=RelayState::Idle;
static bool g_recovering=false;
static struct {bool in_flight=false;} g_match;
static sockaddr_in g_node_endpoints[NODE_CATALOG_COUNT];
static u32 g_host_node=0,g_relay_region=0,g_gameplay_generation=1;
static constexpr u32 RelayRegionCommunity=2,RelayRegionDefault=0;
static char g_error[64]{};
static bool ArmedLocked(){return false;}
static void ClearSelectionLocked(){}
static Result ResultRelayFailed(){return 1;}
static Result ResultCommunityUnreachable(){return 2;}
static bool reachable=true;
static bool ResolveNodeEndpoint(u32 node,sockaddr_in*out,selection_trace::Handle trace){
 selection_trace::Record(trace,"fixture.resolve.begin");
 *out={};out->sin_addr.s_addr=htonl(0x08080808+node);out->sin_port=htons(11451);
 selection_trace::Record(trace,"fixture.resolve.end");return true;
}
static bool CommunityReachable(const sockaddr_in&){return reachable;}
namespace lanplay {
struct Endpoint{u32 ipv4;uint16_t port;};
static bool Probe(Endpoint,unsigned,selection_trace::Handle trace){
 selection_trace::Record(trace,"fixture.probe");return reachable;
}
}
/* SELECTION */
static void reset(){calls=fail_at=opens=closes=flushes=0;exists=false;file.clear();std::memset(tls,0xa5,0x100);errno=EDOM;}
static void assert_tls(){for(auto byte:tls)assert(byte==0xa5);assert(errno==EDOM);}
int main(){
 reset();assert(SetHostNode(2)==0&&g_host_node==2);assert_tls();assert(opens==closes);
 assert(file.find("module=0.8.8-test node=2 seq=0 stage=host.begin")!=std::string::npos);
 assert(file.find("stage=host.committed")!=std::string::npos);
 const unsigned success_calls=calls;
 assert(flushes>=5);
 for(unsigned failure=1;failure<=success_calls;failure++){
  reset();fail_at=failure;g_host_node=0;
  assert(SetHostNode(2)==0&&g_host_node==2);assert_tls();assert(opens==closes);
 }
 // Existing numeric nodes retain the same selection result with logging down.
 for(unsigned node=0;node<2;node++){
  reset();fail_at=2;assert(SetHostNode(node)==0&&g_host_node==node);assert_tls();assert(opens==closes);
 }
 reset();reachable=false;g_host_node=0;assert(SetHostNode(2)==2&&g_host_node==0);
 assert(file.find("stage=host.probe.failed")!=std::string::npos);reachable=true;
 // A second selection replaces the old record; a delayed old writer cannot append.
 reset();auto old=selection_trace::Begin(2,"host.begin");auto current=selection_trace::Begin(3,"room.begin");
 const auto latest=file;selection_trace::Record(old,"late.old");assert(file==latest);
 selection_trace::Record(current,"room.committed");assert(file.find("node=2")==std::string::npos);
 assert(file.find("stage=room.committed")!=std::string::npos);assert_tls();
 // Failed write disables additional I/O until the next operation.
 reset();current=selection_trace::Begin(2,"host.begin");fail_at=calls+2;
 selection_trace::Record(current,"dns.lookup.begin");const auto stopped=calls;
 selection_trace::Record(current,"dns.lookup.end");assert(calls==stopped);assert(opens==closes);
 fail_at=0;assert(SetHostNode(3)==0&&g_host_node==3);assert(file.find("host.committed")!=std::string::npos);
 reset();current=selection_trace::Begin(4,"host.begin");
 std::thread first([&]{for(int i=0;i<50;i++)selection_trace::Record(current,"first");});
 std::thread second([&]{for(int i=0;i<50;i++)selection_trace::Record(current,"second");});
 first.join();second.join();assert(file.size()<=8192&&opens==closes);
 assert_tls();
 puts("PASS actual node-selection trace: per-call SD failures preserve selection, latest operation only, TLS/errno, Flush, concurrency and 8 KiB bound");
}
'''.replace('/* LOGGER */', logger).replace('/* SELECTION */', select)
with tempfile.TemporaryDirectory(prefix='native-selection-trace-') as temporary:
    directory = Path(temporary)
    source, executable = directory / 'test.cpp', directory / 'test'
    source.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-I' + str(ROOT / 'common'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', str(executable)
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
