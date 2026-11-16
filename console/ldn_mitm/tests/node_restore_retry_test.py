#!/usr/bin/env python3
"""Exercise the actual preference loader and retry worker with controlled network timing."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
src=(root/'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
worker=extract.function(src,'void PreferenceRetryWorker(void *argument) {')
restore=extract.function(src,'void RestoreHostPreference() {')
code=r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cassert>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using Result=int;
#define R_SUCCEEDED(r) ((r)==0)
namespace os {struct SdkMutex:std::mutex{};struct TimeSpan {static int FromMilliSeconds(int ms){return ms;}};static void SleepThread(int);}
using TimeSpan=os::TimeSpan;
namespace util {static void Strlcpy(char*a,const char*b,size_t n){std::snprintf(a,n,"%s",b);}}
namespace custom_nodes {static bool SameEndpoint(const NodeDynamicEntry&a,const NodeDynamicEntry&b){return a.port==b.port&&!strcmp(a.host,b.host);}}
enum class NodePreferenceRead{Missing,Valid,Invalid};
static std::atomic<u64> g_host_preference_generation{1};static std::atomic<bool> g_preference_loaded{false},g_retry_running{false};
static std::mutex g_lock,g_catalog_lock,g_restore_lock,g_retry_lifecycle_lock;
static bool g_retry_requested=false,queue_retry=false;
static NodeDynamicCatalog g_dynamic_nodes{};static NodeDynamicEntry g_selected_node{},saved{};
static u32 g_host_node=0,g_selected_status=NodeHostSelected,g_relay_region=0;
static constexpr u32 RelayRegionCommunity=1,RelayRegionDefault=0;
static unsigned scheduled=0,attempts=0,sleeps=0;static bool online=false,manual_change=false,active_game=false;
static NodePreferenceRead read_status=NodePreferenceRead::Missing;
static NodePreferenceRead ReadNodePreference(NodeDynamicEntry*out){*out=saved;return read_status;}
static size_t NodeSlot(u32 n){return n<NODE_SHARED_CAPACITY?n:(n>=NODE_LOCAL_BASE&&n<NODE_LOCAL_BASE+NODE_LOCAL_CAPACITY?NODE_SHARED_CAPACITY+n-NODE_LOCAL_BASE:NODE_TOTAL_CAPACITY);}
static bool RefreshDynamicNodes(){return online;}
static bool LookupNode(u32 id,NodeDynamicEntry*out){if(!online||id!=saved.id)return false;*out=saved;return true;}
static Result SetHostNodeInternal(u32 node,u64 generation){attempts++;if(active_game||g_host_preference_generation!=generation)return 1;g_host_node=node;g_selected_status=NodeHostReady;return 0;}
static void StartPreferenceRetry(){scheduled++;}
namespace os {static void SleepThread(int){sleeps++;if(manual_change){++g_host_preference_generation;manual_change=false;if(queue_retry){g_retry_requested=true;online=true;}}else online=true;}}
''' + worker + '\n' + restore + r'''
static void reset(){g_preference_loaded=false;g_retry_running=false;g_retry_requested=queue_retry=false;g_host_preference_generation=1;g_host_node=0;g_selected_status=NodeHostSelected;scheduled=attempts=sleeps=0;online=manual_change=active_game=false;saved={};g_selected_node={};read_status=NodePreferenceRead::Missing;}
int main(){
 reset();RestoreHostPreference();assert(g_host_node==0&&g_selected_status==NodeHostSelected&&scheduled==0);
 reset();read_status=NodePreferenceRead::Invalid;RestoreHostPreference();assert(g_host_node==NODE_ID_INVALID&&g_selected_status==NodeHostUnavailable&&scheduled==0);
 reset();read_status=NodePreferenceRead::Valid;saved.id=13;saved.protocol=2;saved.flags=1;saved.port=11451;strcpy(saved.host,"relay.example");strcpy(saved.name,"Relay");
 RestoreHostPreference();assert(g_host_node==13&&g_selected_status==NodeHostUnavailable&&scheduled==1);
 PreferenceRetryWorker((void*)1);assert(online&&attempts==1&&g_host_node==13&&g_selected_status==NodeHostReady&&sleeps>0);
 reset();read_status=NodePreferenceRead::Valid;saved.id=13;saved.protocol=2;saved.flags=1;saved.port=11451;strcpy(saved.host,"relay.example");strcpy(saved.name,"Relay");
 RestoreHostPreference();manual_change=true;PreferenceRetryWorker((void*)1);assert(attempts==0&&g_host_preference_generation==2);
 reset();read_status=NodePreferenceRead::Valid;saved.id=13;saved.protocol=2;saved.flags=1;saved.port=11451;strcpy(saved.host,"relay.example");strcpy(saved.name,"Relay");
 RestoreHostPreference();manual_change=true;queue_retry=true;PreferenceRetryWorker((void*)1);
 assert(attempts==1&&g_host_preference_generation==2&&g_selected_status==NodeHostReady&&!g_retry_running&&!g_retry_requested);
 puts("PASS first boot, corrupt preference fail-closed, offline boot/recovery and user-selection generation");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-node-restore-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(root/'common'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
