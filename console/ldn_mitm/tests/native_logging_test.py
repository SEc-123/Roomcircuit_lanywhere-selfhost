#!/usr/bin/env python3
"""Run actual diagnostic logging with failed SD operations and concurrent callers."""
from pathlib import Path
import importlib.util,os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
bsd=extract.function((ROOT/'console/ldn_mitm/source/bsd_spy.cpp').read_text(),'void WriteLine(')
source=(ROOT/'console/ldn_mitm/source/debug.cpp').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code=r'''
#include <algorithm>
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
using u8=uint8_t;using u32=uint32_t;using s64=int64_t;
#define R_TRY(v) do{auto result=(v);if(result)return result;}while(0)
#define R_FAILED(v) ((v)!=0)
#define R_SUCCEEDED(v) ((v)==0)
template<class F>struct ScopeExit{F f;~ScopeExit(){f();}};struct ExitMaker{};template<class F>ScopeExit<F>operator+(ExitMaker,F f){return{f};}
#define ON_SCOPE_EXIT auto scope_exit_##__LINE__=ExitMaker{}+[&]()
#undef ON_SCOPE_EXIT
#define JOIN_A(a,b) a##b
#define JOIN_B(a,b) JOIN_A(a,b)
#define ON_SCOPE_EXIT auto JOIN_B(scope_exit_,__LINE__)=ExitMaker{}+[&]()
void*armGetTls(){static thread_local u8 tls[0x100];return tls;}
static unsigned opened=0,closed=0;static int fault=0;static bool exists=false;static std::string file;
namespace ams {
using Result=int;
namespace os {
struct Mutex:std::recursive_mutex{Mutex(bool){}};
struct ThreadType{};ThreadType*GetCurrentThread(){static ThreadType thread;return &thread;}
unsigned long GetThreadId(const ThreadType*){return 7;}
struct Tick{Tick ToTimeSpan(){return{};}unsigned long GetMilliSeconds()const{return 1;}};
Tick GetSystemTick(){return{};}
}
namespace util {
int TSNPrintf(char*out,size_t cap,const char*format,...){va_list args;va_start(args,format);int n=vsnprintf(out,cap,format,args);va_end(args);return n;}
int TVSNPrintf(char*out,size_t cap,const char*format,va_list args){return vsnprintf(out,cap,format,args);}
int VSNPrintf(char*out,size_t cap,const char*format,va_list args){return vsnprintf(out,cap,format,args);}
}
namespace fs {
using FileHandle=int;constexpr int OpenMode_Write=1,OpenMode_AllowAppend=2;struct WriteOption{static constexpr int Flush=1;};
Result HasFile(bool*out,const char*){*out=exists;return fault==1?1:0;}
Result CreateDirectory(const char*){return 0;}
Result CreateFile(const char*,s64){exists=true;return fault==2?2:0;}
Result OpenFile(FileHandle*out,const char*,int){if(fault==3)return 3;*out=1;opened++;return 0;}
void CloseFile(FileHandle){closed++;}
Result GetFileSize(s64*out,FileHandle){*out=file.size();return fault==4?4:0;}
Result SetFileSize(FileHandle,s64 n){if(fault==5)return 5;file.resize(n);return 0;}
Result WriteFile(FileHandle,s64 offset,const void*data,size_t n,int){if(fault==6)return 6;assert(offset>=0);file.resize(offset+n);std::memcpy(&file[offset],data,n);return 0;}
}
namespace log {Result Initialize();void LogFormatImpl(const char*,...);}
}
/* SOURCE */
namespace fs=ams::fs;namespace util=ams::util;
#define RC_LEGACY_DIR "sdmc:/lanywhere"
#define RC_V1_DIR "sdmc:/switch/RoomCircuit/data"
#define RC_CURRENT_DIR "sdmc:/config/RoomCircuit/data"
namespace storage {bool StorageUseCurrent(){return false;} unsigned StorageGeneration(){ return 0; }}
const char *rc_storage_path_gen(unsigned gen,const char*oldPath,const char*v1,const char*newPath){return gen==3?newPath:gen==1?v1:oldPath;}
static const char*LogPath(){return "sdmc:/lanywhere/bsd.log";}
static bool DebugEnabled(){return true;}
/* BSD */
int main(){
 for(int failure=1;failure<=6;failure++) {
  exists=false;file.clear();fault=failure;
  if(failure<=3)assert(SetLogging(1)!=0);else {assert(SetLogging(1)==0);if(failure==5)file.resize(256*1024);ams::log::LogFormatImpl("line");}
  u32 enabled=9;GetLogging(&enabled);assert(!enabled&&opened==closed);
 }
 fault=0;file.assign(256*1024,'x');assert(SetLogging(1)==0);
 std::vector<u8> bytes(100,0x5a);
 std::thread a([&]{for(int i=0;i<30;i++)ams::log::LogHexImpl(bytes.data(),bytes.size());});
 std::thread b([&]{for(int i=0;i<30;i++)ams::log::LogFormatImpl("message %d\n",i);});
 a.join();b.join();assert(opened==closed&&file.size()<=256*1024);
 ams::log::Finalize();u32 enabled;GetLogging(&enabled);assert(!enabled);
 fault=0;file.assign(256*1024,'x');WriteLine("socket %d",7);assert(file.size()<256&&opened==closed);
 fault=5;file.assign(256*1024,'x');WriteLine("socket %d",7);assert(file.size()==256*1024&&opened==closed);
 puts("PASS actual diagnostic logger: SD failure disables logging, handles close, concurrent hex/text and bounded file");
}
'''.replace('/* SOURCE */',source).replace('/* BSD */',bsd)
with tempfile.TemporaryDirectory(prefix='native-logging-') as tmp:
 cpp=Path(tmp)/'test.cpp';cpp.write_text(code);binary=Path(tmp)/'test'
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=10)
