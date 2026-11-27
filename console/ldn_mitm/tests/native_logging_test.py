#!/usr/bin/env python3
"""Run actual diagnostic logging with failed SD operations and concurrent callers."""
from pathlib import Path
import importlib.util,os,re,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
bsd=extract.function((ROOT/'console/ldn_mitm/source/bsd_spy.cpp').read_text(),'void WriteLine(')
source=(ROOT/'console/ldn_mitm/source/debug.cpp').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
accept_header = (ROOT/'console/ldn_mitm/source/ldn_accept_trace.hpp').read_text()
accept_source = (ROOT/'console/ldn_mitm/source/ldn_accept_trace.cpp').read_text()
accept = '\n'.join(line for line in (accept_header + '\n' + accept_source).splitlines()
                  if not line.startswith(('#include', '#pragma once')))
root_service = (ROOT/'console/ldn_mitm/source/ldnmitm_service.hpp').read_text()
query = extract.function(root_service, 'static bool ShouldMitm(')
bsd_service = (ROOT/'console/ldn_mitm/source/bsd_spy.hpp').read_text()
bsd_query = extract.function(bsd_service, 'static bool ShouldMitm(')
assert 'LogFormat' not in query
sdk_ids = (ROOT/'console/Atmosphere-libs/libstratosphere/include/stratosphere/ncm/ncm_content_meta_id.hpp').read_text()
application_check = extract.function(sdk_ids, 'inline constexpr bool IsApplicationId(const ProgramId &')
first_app, last_app = re.findall(r'ApplicationId::(?:Start|End)\s*=\s*\{\s*(0x[0-9a-fA-F]+)', sdk_ids)[:2]
code=r'''
#include <algorithm>
#include <atomic>
#include <array>
#include <cinttypes>
#include <condition_variable>
#include <chrono>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
using u64=uint64_t;using u8=uint8_t;using u32=uint32_t;using s64=int64_t;
#define GITDESCVER "test"
#define MAKERESULT(m,d) (((m)&0x1ff)|((d)<<9))
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
struct TimeSpan {static int FromSeconds(int s){return s;}};
namespace os {
// Rendezvous immediately before a blocking lock, or immediately after try_lock.
thread_local std::atomic<bool> *observed_lock=nullptr;
struct SdkMutex {
 std::mutex mutex;
 void lock(){if(observed_lock)observed_lock->store(true);mutex.lock();}
 void unlock(){mutex.unlock();}
 bool try_lock(){const bool acquired=mutex.try_lock();if(observed_lock)observed_lock->store(true);return acquired;}
};
struct SdkConditionVariable {
 std::condition_variable_any cv;
 void TimedWait(SdkMutex &m,int){cv.wait_for(m,std::chrono::milliseconds(5));}
 void Signal(){cv.notify_one();}
};
struct Mutex:std::recursive_mutex{Mutex(bool){}};
struct ThreadType{std::atomic<bool> stopped{false};}; bool TryWaitThread(ThreadType *t){return t->stopped.load();} ThreadType*GetCurrentThread(){static ThreadType thread;return &thread;}
thread_local unsigned long test_tid=7;
unsigned long GetThreadId(const ThreadType*){return test_tid;}
std::atomic<s64> test_ms{1}, writer_observed_ms{-1};
struct Tick{Tick ToTimeSpan(){return{};}s64 GetMilliSeconds()const{
 const auto n=test_ms.load();if(test_tid==19)writer_observed_ms.store(n);return n;
}};
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
std::atomic<bool> block_close{false},close_entered{false},release_close{false};
std::atomic<unsigned> writer_ready{0},writer_alive{0};
void CloseFile(FileHandle){
 ++closed;
 if(block_close && file.find("stage=ack.begin")!=std::string::npos){close_entered=true;while(!release_close)std::this_thread::yield();}
}
Result GetFileSize(s64*out,FileHandle){*out=file.size();return fault==4?4:0;}
Result SetFileSize(FileHandle,s64 n){if(fault==5)return 5;file.resize(n);return 0;}
Result WriteFile(FileHandle,s64 offset,const void*data,size_t n,int){if(fault==6)return 6;assert(offset>=0);file.resize(offset+n);std::memcpy(&file[offset],data,n);
 std::string_view line(static_cast<const char*>(data),n);
 if(line.find("ldn.writer.ready ")!=line.npos)++writer_ready;
 if(line.find("ldn.writer.alive ")!=line.npos)++writer_alive;
 return 0;}
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
#define constinit
#define LogFormat(fmt,...) ::ams::log::LogFormatImpl(fmt "\n", ##__VA_ARGS__)
/* ACCEPT TRACE */
namespace ams::ncm {
struct ProgramId { u64 value; constexpr operator u64() const {return value;} };
struct ApplicationId { static constexpr u64 Start=/* FIRST APP */, End=/* LAST APP */; };
/* APPLICATION CHECK */
}
namespace ams::sm {
struct MitmProcessInfo {struct {u64 value;} process_id; ncm::ProgramId program_id;};
}
namespace ams::mitm::ldn {
struct QueryRoot { /* SHOULD MITM */ };
}
namespace ams::mitm::bsd {
struct QueryRoot { /* BSD SHOULD MITM */ };
}
int main(){
 for(int failure=1;failure<=6;failure++) {
  exists=false;file.clear();fault=failure;
  if(failure==5)file.assign(256*1024,'x');
  assert(SetLogging(1)!=0);
  u32 enabled=9;GetLogging(&enabled);assert(!enabled&&opened==closed);
 }
 // Successful enable writes the actual build to the retained log before returning.
 fault=0;file="old trace\n";assert(SetLogging(1)==0);
 assert(file.find("old trace\n")==0 && file.find("ldn.logging version=test session=1 enabled=1")!=std::string::npos);
 const auto first_marker=file;assert(SetLogging(1)==0 && file==first_marker);
 fault=6;ams::log::LogFormatImpl("write failure");u32 disabled{};GetLogging(&disabled);assert(disabled==0);
 fault=0;file.assign(256*1024,'x');assert(SetLogging(1)==0);
 std::vector<u8> bytes(100,0x5a);
 std::thread a([&]{for(int i=0;i<30;i++)ams::log::LogHexImpl(bytes.data(),bytes.size());});
 std::thread b([&]{for(int i=0;i<30;i++)ams::log::LogFormatImpl("message %d\n",i);});
 a.join();b.join();assert(opened==closed&&file.size()<=256*1024);
 ams::log::Finalize();u32 enabled;GetLogging(&enabled);assert(!enabled);
 fault=0;file.assign(256*1024,'x');WriteLine("socket %d",7);assert(file.size()<256&&opened==closed);
 fault=5;file.assign(256*1024,'x');WriteLine("socket %d",7);assert(file.size()==256*1024&&opened==closed);
 // A flushed line can be visible while CloseFile still holds the SD logger lock.
 // The MITM query never logs, and the accept worker continues through that stall.
 file.clear(); fault=0;
 ams::os::ThreadType server{};
 ams::log::LdnAcceptInfo info{143,0x01006f8002326000ull,128,4096,48};
 ams::log::RecordLdnAccept("disabled",info);
 assert(SetLogging(1)==0);
 {
  const char *stages[]={"ack.done","object.ready","slot.reserved","kernel.accepted","wait.registering","accept.done"};
  ams::log::RecordLdnAccept("ack.begin",info);
  for(const auto *stage:stages){
   std::atomic<bool> attempted{false};
   std::unique_lock consumer(ams::log::g_lock);
   std::thread producer([&]{
    ams::os::observed_lock=&attempted;
    ams::log::RecordLdnAccept(stage,info);
   });
   while(!attempted.load())std::this_thread::yield();
   consumer.unlock();producer.join();
  }
  ams::os::ThreadType finished{};finished.stopped=true;
  ams::log::DrainLdnAcceptTraceUntilExit(&finished);
  for(const auto *stage:stages){
   if(file.find(std::string("stage=")+stage+" ")==std::string::npos)std::fprintf(stderr,"%s",file.c_str());
   assert(file.find(std::string("stage=")+stage+" ")!=std::string::npos);
  }
  assert(file.find("dropped=6")==std::string::npos);
  puts("PASS six contended RAM writes preserve complete LDN accept trace");
  file.clear();
 }
 // Undrained records from a previous enable period must not follow a fresh marker.
 ams::log::RecordLdnAccept("stale",info);
 assert(SetLogging(0)==0 && SetLogging(1)==0);
 fs::block_close=true;
 ams::log::RecordLdnAccept("ack.begin",info);
 std::thread writer([&]{ams::log::DrainLdnAcceptTraceUntilExit(&server);});
 while(!fs::close_entered)std::this_thread::yield();
 assert(file.find("stage=ack.begin")!=std::string::npos && file.find("stage=disabled")==std::string::npos && file.find("stage=stale")==std::string::npos);
 for(int i=0;i<72;i++)ams::log::RecordLdnAccept("kernel.accepted",info);
 ams::log::RecordLdnAccept("accept.done",info,0xfeed);
 server.stopped=true;fs::release_close=true;writer.join();
 assert(file.find("stage=accept.done pid=143 title=01006f8002326000 rc=0000feed pointer=128/4096 max_sessions=48")!=std::string::npos);
 assert(file.find("dropped=9")!=std::string::npos);
 puts("PASS accept worker records progress/returned error during stalled SD close; bounded queue retains last stage");
 // Observers never see a mixed pair when multiple queries precede the next writer turn.
 file.clear();
 server.stopped=true;
 assert(SetLogging(0)==0);
 auto request=[](u64 pid,u64 title){assert(ams::mitm::ldn::QueryRoot::ShouldMitm({{pid},{title}}));};
 request(1,0x01006f8002326000ULL);
 assert(SetLogging(1)==0);
 request(142,0x010047700d540000ULL);  // Clubhouse Games
 request(143,0x01006f8002326000ULL);  // Animal Crossing
 request(144,0x010000000000100dULL);  // Album
 request(145,0x420000000007e51aULL);  // Overlay loader
 assert(ams::mitm::bsd::QueryRoot::ShouldMitm({{146},{0x010047700d540000ULL}}));
 assert(!ams::mitm::bsd::QueryRoot::ShouldMitm({{144},{0x010000000000100dULL}}));
 assert(!ams::mitm::bsd::QueryRoot::ShouldMitm({{145},{0x420000000007e51aULL}}));
 auto bsd_info = info; bsd_info.bsd_service = true;
 ams::log::RecordLdnAccept("accept.done",bsd_info,0);
 ams::log::DrainLdnAcceptTraceUntilExit(&server);
 assert(file.find("bsd.query version=test seq=1 pid=146 title=010047700d540000 skipped=0")!=std::string::npos);
 assert(file.find("bsd.accept version=test")!=std::string::npos);
 assert(file.find("ldn.query version=test seq=2 pid=143 title=01006f8002326000 skipped=1")!=std::string::npos);
 assert(file.find("pid=142")==std::string::npos);
 {
  // No game, SM query, accept, or explicit reconnect during this test.
  file.clear(); assert(SetLogging(0)==0);
  const unsigned ready=fs::writer_ready,alive=fs::writer_alive;
  fs::block_close=false;
  ams::os::ThreadType idle{};
  std::thread idle_writer([&]{ams::os::test_tid=19;ams::log::DrainLdnAcceptTraceUntilExit(&idle);});
  assert(SetLogging(1)==0);
  const auto epoch=ams::log::LoggingSession();
  const auto wait_until=[](auto test){
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
   while(!test() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
   assert(test());
  };
  wait_until([&]{return fs::writer_ready.load()==ready+1;});
  ams::os::test_ms=2000;wait_until([&]{return ams::os::writer_observed_ms.load()==2000;});
  assert(fs::writer_alive.load()==alive);
  ams::os::test_ms=2001;wait_until([&]{return fs::writer_alive.load()==alive+1;});
  ams::os::test_ms=2010;wait_until([&]{return ams::os::writer_observed_ms.load()==2010;});
  assert(fs::writer_alive.load()==alive+1);
  assert(SetLogging(0)==0);ams::os::test_ms=9000;assert(SetLogging(1)==0);
  wait_until([&]{return fs::writer_ready.load()==ready+2;});
  idle.stopped=true;idle_writer.join();
  const std::string marker="ldn.writer.ready version=test session="+std::to_string(epoch);
  assert(file.find("t: 19] "+marker)!=std::string::npos);
  assert(fs::writer_alive.load()==alive+1);
  puts("PASS independent writer confirms enabled epoch and reports at most once per two seconds without a game request");
 }
 puts("PASS actual diagnostic logger: SD failure disables logging, handles close, concurrent hex/text and bounded file");
}
'''.replace('/* SOURCE */',source).replace('/* BSD */',bsd).replace('/* ACCEPT TRACE */',accept)
code=code.replace('/* FIRST APP */',first_app).replace('/* LAST APP */',last_app)
code=code.replace('/* APPLICATION CHECK */',application_check).replace('/* SHOULD MITM */',query).replace('/* BSD SHOULD MITM */',bsd_query)
with tempfile.TemporaryDirectory(prefix='native-logging-') as tmp:
 cpp=Path(tmp)/'test.cpp';cpp.write_text(code);binary=Path(tmp)/'test'
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=10)
