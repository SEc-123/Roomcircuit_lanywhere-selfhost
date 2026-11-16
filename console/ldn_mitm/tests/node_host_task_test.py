#!/usr/bin/env python3
"""Run the actual sysmodule-owned host task with controlled thread and route boundaries."""
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
functions = '\n'.join(extract.function(source, sig) for sig in (
    'u64 BeginManualHostSelection(', 'void FinishManualHostSelection(', 'void HostNodeTaskWorker(',
    'Result StartHostNodeTask(', 'NodeHostTaskStatus GetHostNodeTask('))
harness = r'''
#include "node_lobby.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
struct Result { u32 value; u32 GetValue() const{return value;} };
#define R_FAILED(x) ((x).value!=0)
#define R_SUCCEED() return Result{0}
static Result ResultRelayFailed(){return {1};}
namespace os {
using SdkMutex=std::mutex; constexpr size_t MemoryPageSize=4096;
struct ThreadType { std::thread *thread=nullptr; void (*entry)(void*)=nullptr; void *arg=nullptr; std::atomic<bool> done{false}; };
static bool fail_create=false,random_zero=false;
static u64 random_seed=100;
static void GenerateRandomBytes(void *out,size_t n){assert(n==sizeof(u64));const u64 value=random_zero?0:random_seed;std::memcpy(out,&value,n);}
static Result CreateThread(ThreadType *t,void (*fn)(void*),void *arg,void*,size_t,int){if(fail_create)return {2};t->entry=fn;t->arg=arg;return {0};}
static void StartThread(ThreadType *t){t->done=false;t->thread=new std::thread([t]{t->entry(t->arg);t->done=true;});}
static bool TryWaitThread(ThreadType *t){return t->done.load();}
static unsigned destroyed=0;
static void DestroyThread(ThreadType *t){assert(t->thread);t->thread->join();delete t->thread;t->thread=nullptr;destroyed++;}
static void SetThreadNamePointer(ThreadType*,const char*){}
}
static os::SdkMutex g_host_task_lock;
static os::ThreadType g_host_task_thread;
alignas(os::MemoryPageSize) static u8 g_host_task_stack[0x4000];
static bool g_host_task_created=false;
static NodeHostTaskStatus g_host_task{1,NodeHostTaskIdle,0,0,0,{}};
static u64 g_host_task_next_id=0;
static u64 g_host_task_generation=0;
static std::mutex g_lock;
static std::atomic<u64> g_host_preference_generation{1};
static u64 g_manual_host_generation=0;
static u32 g_selected_status=NodeHostReady,g_host_node=0;
static constexpr u32 NODE_ID_INVALID=UINT32_MAX;
static unsigned retries=0;
static void StartPreferenceRetry(){++retries;}
static NodeDynamicEntry actual{};
static std::atomic<bool> hold{false},entered{false};
static std::atomic<u32> outcome{0};
static bool LookupNode(u32 id,NodeDynamicEntry *out){if(id!=actual.id)return false;*out=actual;return true;}
static Result SetHostNodeInternalExpected(u32 id,u64 generation,const NodeDynamicEntry *identity){entered=true;while(hold)std::this_thread::sleep_for(std::chrono::milliseconds(1));if(generation!=g_host_preference_generation||id!=actual.id||std::memcmp(identity,&actual,sizeof(*identity)))return {3};return {outcome.load()};}
''' + functions + r'''
static void waitDone(){for(int i=0;i<2000;++i){auto state=GetHostNodeTask(0).state;if(state==NodeHostTaskFinished&&!g_host_task_created)return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);}
int main(){actual.id=13;actual.flags=1;actual.port=11451;std::strcpy(actual.host,"example.org");
 assert(GetHostNodeTask(0).state==NodeHostTaskIdle);u64 id=999;
 NodeDynamicEntry bad=actual;bad.port++;assert(R_FAILED(StartHostNodeTask(bad,&id))&&id==0);
 const u64 before_failure=g_host_preference_generation;
 os::fail_create=true;assert(R_FAILED(StartHostNodeTask(actual,&id))&&id==0&&GetHostNodeTask(0).state==NodeHostTaskIdle);
 assert(g_host_preference_generation==before_failure&&g_manual_host_generation==0);
 os::fail_create=false;hold=true;entered=false;assert(!R_FAILED(StartHostNodeTask(actual,&id))&&id);
 for(int i=0;i<2000&&!entered;++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(entered);
 auto working=GetHostNodeTask(id);assert(working.state==NodeHostTaskWorking&&working.task_id==id&&!std::memcmp(&working.identity,&actual,sizeof(actual)));
 u64 rejected=999;assert(R_FAILED(StartHostNodeTask(actual,&rejected))&&rejected==0);
 assert(GetHostNodeTask(id+1).state==NodeHostTaskExpired);
 // The caller can disappear while the sysmodule-owned task completes.
 hold=false;waitDone();assert(GetHostNodeTask(id).result==0&&os::destroyed==1&&!g_host_task_created);
 const u64 previous=id;hold=true;entered=false;assert(!R_FAILED(StartHostNodeTask(actual,&id))&&id>previous);
 for(int i=0;i<2000&&!entered;++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(entered);
 const u64 foreground=BeginManualHostSelection(); // A newer manual selection supersedes this queued task.
 g_selected_status=NodeHostUnavailable;g_host_node=13;
 hold=false;waitDone();assert(GetHostNodeTask(id).result!=0&&GetHostNodeTask(previous).state==NodeHostTaskExpired);
 assert(g_manual_host_generation==foreground&&retries==0); // stale failure must not schedule old retry
 g_selected_status=NodeHostReady;FinishManualHostSelection(foreground,false);assert(!g_manual_host_generation&&!retries);
 const u64 superseded=id;outcome=7;assert(!R_FAILED(StartHostNodeTask(actual,&id))&&id>superseded);waitDone();
 assert(GetHostNodeTask(superseded).state==NodeHostTaskExpired&&GetHostNodeTask(id).result==7);
 assert(!g_host_task_created&&os::destroyed==3);
 // Simulate independent sysmodule restart: a new random seed avoids reuse.
 g_host_task={1,NodeHostTaskIdle,0,0,0,{}};g_host_task_next_id=0;os::random_seed=500;outcome=0;
 assert(!R_FAILED(StartHostNodeTask(actual,&id))&&id!=previous&&id!=superseded);waitDone();
 const u64 new_id=id;assert(GetHostNodeTask(superseded).state==NodeHostTaskExpired);
 g_host_task={1,NodeHostTaskIdle,0,0,0,{}};g_host_task_next_id=0;os::random_zero=true;
 const u64 before_random_failure=g_host_preference_generation;
 assert(R_FAILED(StartHostNodeTask(actual,&id))&&id==0&&!g_host_task_created&&g_host_preference_generation==before_random_failure);
 assert(new_id==500);
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-host-task-') as directory:
    source_file = Path(directory) / 'test.cpp'; binary = Path(directory) / 'test'
    source_file.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++20','-Wall','-Wextra','-Werror','-pthread',
                   '-I'+str(root/'common'),str(source_file),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
print('PASS sysmodule-owned task: stale IDs, busy, creation failure and completion after caller exits')
