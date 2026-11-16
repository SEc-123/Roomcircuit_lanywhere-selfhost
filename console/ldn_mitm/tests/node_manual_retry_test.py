#!/usr/bin/env python3
"""Run real retry/manual arbitration with controlled concurrent probes."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('extract', root / 'console/ldn_mitm/tests/discovery_route_test.py')
extract = importlib.util.module_from_spec(spec); spec.loader.exec_module(extract)
source = (root / 'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
begin = extract.function(source, 'u64 BeginManualHostSelection(')
finish = extract.function(source, 'void FinishManualHostSelection(')
worker = extract.function(source, 'void PreferenceRetryWorker(')
start = extract.function(source, 'void StartPreferenceRetry() {')
harness = r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using Result=int;
#define R_SUCCEEDED(r) ((r)==0)
#define R_FAILED(r) ((r)!=0)
namespace os {
using SdkMutex=std::mutex; constexpr size_t MemoryPageSize=4096;
struct TimeSpan{static int FromMilliSeconds(int){return 1;}};
struct ThreadType{std::thread *thread=nullptr;void(*entry)(void*)=nullptr;void *arg=nullptr;};
static bool fail_create=false;
static int CreateThread(ThreadType*t,void(*f)(void*),void*a,void*,size_t,int){if(fail_create)return 1;t->entry=f;t->arg=a;return 0;}
static void StartThread(ThreadType*t){t->thread=new std::thread(t->entry,t->arg);}
static void WaitThread(ThreadType*t){assert(t->thread);t->thread->join();}
static void DestroyThread(ThreadType*t){delete t->thread;t->thread=nullptr;}
static void SetThreadNamePointer(ThreadType*,const char*){}
static void SleepThread(int){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
using TimeSpan=os::TimeSpan;
namespace custom_nodes {static bool SameEndpoint(const NodeDynamicEntry&a,const NodeDynamicEntry&b){return a.port==b.port&&!std::strcmp(a.host,b.host);}}
static std::mutex g_lock,g_catalog_lock,g_retry_lifecycle_lock;
static std::atomic<u64> g_host_preference_generation{1};
static u64 g_manual_host_generation=0;
static std::atomic<bool> g_retry_running{false};
static bool g_retry_requested=false,g_retry_created=false;
static os::ThreadType g_retry_thread;
alignas(os::MemoryPageSize) static u8 g_retry_stack[0x4000];
static u32 g_selected_status=NodeHostUnavailable,g_host_node=13;
static NodeDynamicEntry g_selected_node{};static NodeDynamicCatalog g_dynamic_nodes{};
static std::atomic<bool> entered{false},hold{false};
static std::atomic<unsigned> old_commits{0};
static bool RefreshDynamicNodes(){return true;}
static bool LookupNode(u32 id,NodeDynamicEntry*out){if(id!=g_selected_node.id)return false;*out=g_selected_node;return true;}
static Result SetHostNodeInternal(u32 id,u64 generation){
 entered=true;while(hold.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
 std::scoped_lock lock(g_lock);
 if(generation!=g_host_preference_generation.load())return 1;
 g_host_node=id;g_selected_status=NodeHostReady;++old_commits;return 0;
}
static void StartPreferenceRetry();
''' + begin + '\n' + finish + '\n' + worker + '\n' + start + r'''
static void waitEntered(){for(int i=0;i<2000&&!entered;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(entered);}
static void waitStopped(){for(int i=0;i<2000&&g_retry_running;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(!g_retry_running);}
static void cleanup(){if(g_retry_created){os::WaitThread(&g_retry_thread);os::DestroyThread(&g_retry_thread);g_retry_created=false;}}
static void reset(){cleanup();g_host_preference_generation=1;g_manual_host_generation=0;g_retry_running=false;g_retry_requested=false;g_selected_status=NodeHostUnavailable;g_host_node=13;g_selected_node={};g_selected_node.id=13;g_selected_node.protocol=2;g_selected_node.flags=1;g_selected_node.port=11451;std::strcpy(g_selected_node.host,"relay.example");entered=false;hold=false;old_commits=0;os::fail_create=false;}
int main(){
 // An already-running restore is superseded by a manual B. Periodic catalog or
 // host-info polling cannot adopt B's generation to restore the old node.
 reset();hold=true;StartPreferenceRetry();waitEntered();const u64 b=BeginManualHostSelection();
 StartPreferenceRetry();assert(!g_retry_requested);hold=false;waitStopped();
 {std::scoped_lock lock(g_lock);g_host_node=7;g_selected_status=NodeHostReady;}
 FinishManualHostSelection(b,false);assert(!g_manual_host_generation&&g_host_node==7&&old_commits==0);
 // Stale A fails while B is probing: it cannot clear B or schedule a retry.
 reset();const u64 a=BeginManualHostSelection();const u64 b2=BeginManualHostSelection();
 FinishManualHostSelection(a,true);assert(g_manual_host_generation==b2&&!g_retry_running);
 {std::scoped_lock lock(g_lock);g_host_node=8;g_selected_status=NodeHostReady;}
 FinishManualHostSelection(b2,false);assert(!g_manual_host_generation&&g_host_node==8&&old_commits==0);
 // B fails and requests retry; a later C cancels that retry and cannot be
 // overwritten by old retry handoff while C remains in flight.
 reset();hold=true;const u64 b3=BeginManualHostSelection();FinishManualHostSelection(b3,true);waitEntered();
 const u64 c=BeginManualHostSelection();StartPreferenceRetry();assert(!g_retry_requested);
 hold=false;waitStopped();{std::scoped_lock lock(g_lock);g_host_node=9;g_selected_status=NodeHostReady;}
 FinishManualHostSelection(c,false);assert(!g_manual_host_generation&&g_host_node==9&&old_commits==0);
 // Thread exhaustion must leave the saved state and generation intact; a
 // later retry can still recover it.
 reset();const auto before=g_host_preference_generation.load();os::fail_create=true;StartPreferenceRetry();
 assert(!g_retry_running&&!g_retry_created&&g_host_preference_generation==before&&g_host_node==13);
 os::fail_create=false;StartPreferenceRetry();waitStopped();assert(g_host_node==13&&g_selected_status==NodeHostReady&&old_commits==1);
 cleanup();
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-manual-retry-') as directory:
    path = Path(directory); (path/'test.cpp').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread',
                   '-I'+str(root/'common'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
print('PASS manual-vs-retry arbitration, supersession, handoff and thread exhaustion')
