#!/usr/bin/env python3
"""Inject worker creation failures into the actual relay session preparation."""
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
source = (root / 'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
prepare = extract.function(source, 'Result PrepareRelayWorker(')
enter = extract.function(source, 'Result Enter(')
assert enter.index('PrepareRelayWorker(s, lk)') < enter.index('Install(s)')
assert enter.index('PrepareRelayWorker(s, lk)') < enter.index('g_state = RelayState::InRoom')
assert 'R_ABORT_UNLESS(os::CreateThread' not in enter
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
using Result=int;
#define R_SUCCEEDED(r) ((r)==0)
#define R_SUCCEED() return 0
namespace os {
using SdkMutex=std::mutex; struct ThreadType{};
static bool fail=true; static unsigned created=0;
static Result CreateThread(ThreadType*,void(*)(void*),void*,void*,size_t,int){++created;return fail?7:0;}
}
struct Session{int tcp,udp;};
static os::ThreadType g_thread;static char g_thread_stack[16384];
static constexpr int T_Leave=7;
static unsigned sent=0,closed=0;static int handles[2];static char error[128];static std::mutex g_lock;
static void Worker(void*){}
static int ResultRelayFailed(){return 42;}
static bool SendFrameTo(int fd,int type,const void*,size_t,const void*,size_t){
 assert(fd==101&&type==T_Leave&&g_lock.try_lock());g_lock.unlock();++sent;return true;
}
static int fake_close(int fd){assert(g_lock.try_lock());g_lock.unlock();handles[closed++]=fd;return 0;}
#define close fake_close
static void SetError(const char *message){assert(!g_lock.try_lock());std::snprintf(error,sizeof(error),"%s",message);}
''' + prepare + r'''
int main(){
 Session session{101,102}; std::unique_lock<os::SdkMutex> lock(g_lock);
 assert(PrepareRelayWorker(session,lock)==42 && lock.owns_lock());
 assert(sent==1&&closed==2&&handles[0]==101&&handles[1]==102&&session.tcp==-1&&session.udp==-1);
 assert(!std::strcmp(error,"Could not start the room connection. Try again."));
 os::fail=false; Session retry{201,202};
 assert(PrepareRelayWorker(retry,lock)==0&&lock.owns_lock());
 assert(retry.tcp==201&&retry.udp==202&&os::created==2&&sent==1&&closed==2);
 puts("PASS thread exhaustion leaves room, closes uninstalled sockets and permits retry");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-room-thread-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror',
                   '-pthread',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
