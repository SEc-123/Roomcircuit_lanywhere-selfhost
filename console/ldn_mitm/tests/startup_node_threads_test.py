#!/usr/bin/env python3
"""Check the real startup and optional thread helpers with exhausted thread slots."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('discovery_route_test.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
main = (root / 'console/ldn_mitm/source/ldnmitm_main.cpp').read_text()
relay = (root / 'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
startup = extract.function(main, 'void ProcessForServerOnAllThreads(void *)')
catalog = extract.function(relay, 'void RequestCatalogRefresh() {')
retry = extract.function(relay, 'void StartPreferenceRetry() {')
initialize = extract.function(relay, 'void InitializeNodeSelection() {')
assert 'InitializeNodeSelection()' not in extract.function(main, 'void Main()')
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
using u8=uint8_t; using u64=uint64_t; using s32=int; using Result=int;
#define R_FAILED(r) ((r)!=0)
#define R_ABORT_UNLESS(r) assert((r)==0)
namespace os {
struct ThreadType{}; using SdkMutex=std::mutex;
static unsigned mandatory=0, optional=0, started=0, waited=0;
static Result CreateThread(ThreadType *, void(*)(void*), void *, void *, size_t, s32);
static s32 GetThreadCurrentPriority(void*) { return 6; }
static void *GetCurrentThread() { return nullptr; }
static void SetThreadNamePointer(ThreadType*,const char*) {}
static void StartThread(ThreadType*) { ++started; }
static void WaitThread(ThreadType*) { ++waited; }
static void DestroyThread(ThreadType*) {}
}
constexpr size_t NumExtraThreads=2, ThreadStackSize=16384;
static u8 g_extra_thread_stacks[NumExtraThreads][ThreadStackSize], g_catalog_stack[16384], g_retry_stack[16384];
static os::ThreadType g_extra_threads[NumExtraThreads],g_catalog_thread,g_retry_thread;
static std::mutex g_catalog_thread_lock,g_retry_lifecycle_lock;
static std::atomic<bool> g_catalog_running{false},g_retry_running{false};
static std::atomic<u64> g_host_preference_generation{1};
static bool g_catalog_created=false,g_retry_created=false,g_retry_requested=false;
static u64 g_catalog_next_refresh=0;
static u64 SelectionNowMs(){return 100;}
static void CatalogRefreshWorker(void*){}
static void PreferenceRetryWorker(void*){}
static int loops=0;
static void LoopServerThread(void*){++loops;}
namespace os {
static Result CreateThread(ThreadType *thread,void(*)(void*),void*,void*,size_t,s32){
    if(thread==&g_extra_threads[0]||thread==&g_extra_threads[1]){assert(optional==0);++mandatory;return 0;}
    assert(mandatory==NumExtraThreads);++optional;return 1; // Resource limit after mandatory workers.
}}
''' + catalog + '\n' + retry + r'''
static void RestoreHostPreference(){StartPreferenceRetry();}
namespace mitm::lanywhere {
''' + initialize + r'''
}
''' + startup + r'''
int main(){
    ProcessForServerOnAllThreads(nullptr);
    assert(os::mandatory==2&&os::optional==2&&os::started==2&&loops==1&&os::waited==2);
    assert(!g_retry_running&&!g_retry_created&&!g_catalog_running&&!g_catalog_created);
    puts("PASS mandatory IPC workers precede optional node threads; optional resource failure is nonfatal");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-startup-threads-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-pthread', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
