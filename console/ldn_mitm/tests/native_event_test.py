#!/usr/bin/env python3
"""Exercise the game-facing LDN setup contract and event ownership."""
from pathlib import Path
import importlib.util,os,re,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
source=(ROOT/'console/ldn_mitm/source/ldn_icommunication.cpp').read_text()
header=(ROOT/'console/ldn_mitm/source/ldn_icommunication.hpp').read_text()
interface=(ROOT/'console/ldn_mitm/source/interfaces/icommunication.hpp').read_text()
commands=interface[interface.index('#define AMS_LDN_ICOMMUNICATION'):interface.index('AMS_SF_DEFINE_INTERFACE(')]
assert re.search(r'AMS_SF_METHOD_INFO\(C,\s*H,\s*106\s*,',commands), 'Missing game-facing LDN command 106'
code=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <cstdio>
#include <string_view>
using Result=int;
using u32=uint32_t;
#define R_TRY(v) do{auto result=(v);if(result)return result;}while(0)
#define R_FAILED(v) ((v)!=0)
#define MAKERESULT(m,d) ((m)*1000+(d))
#define LogFormat(...) ((void)0)
Result ResultSuccess(){return 0;}
static bool fail_event=false,fail_initialize=false;
static std::atomic<unsigned>live_events{0},signals{0};
namespace ams {
namespace os {
constexpr int EventClearMode_AutoClear=0;
struct SystemEvent {
 bool active=false;
 SystemEvent(){live_events++;}~SystemEvent(){active=false;live_events--;}
 SystemEvent*GetBase(){return this;}
 int GetReadableHandle()const{assert(active);return 17;}
 void Signal(){assert(active);signals++;}
};
Result CreateSystemEvent(SystemEvent*p,int,bool){if(fail_event)return 7;p->active=true;return 0;}
}
namespace sf {
struct CopyHandle{};using ClientProcessId=uint64_t;
template<class T>struct Out {int *out;void SetValue(int value,bool)const{*out=value;}};
}
namespace mitm::ldn {
struct Discovery {
 std::thread thread;std::atomic<bool>stop{false};
 Result initialize(std::function<void()>callback){
  if(fail_initialize)return 9;
  if(thread.joinable())return 0;
  stop=false;thread=std::thread([this,callback]{while(!stop){callback();std::this_thread::yield();}});return 0;
 }
 Result finalize(){stop=true;if(thread.joinable())thread.join();return 0;}
 Result openAccessPoint(){return thread.joinable()?0:13;}
 Result openStation(){return thread.joinable()?0:13;}
};
struct ICommunicationService {
 Discovery lanDiscovery;os::SystemEvent *state_event=nullptr;
 std::mutex lifecycle_mutex,event_mutex;
 Result Initialize(const sf::ClientProcessId&);Result Finalize();Result AttachStateChangeEvent(sf::Out<sf::CopyHandle>);
 Result SetProtocol(u32);Result OpenAccessPoint();Result OpenStation();
 void onEventFired();
 /* DESTRUCTOR */
};
/* FUNCTIONS */
}
}
/* COMMANDS */
struct Command {u32 id;std::string_view method;std::string_view arguments;};
#define AMS_SF_METHOD_INFO(C,H,id,result,method,args,forward,...) {id,#method,#args},
constexpr Command ldnCommands[]={AMS_LDN_ICOMMUNICATION(0,0)};
#undef AMS_SF_METHOD_INFO
constexpr const Command *lookup(u32 id){
 for(const auto &command:ldnCommands)if(command.id==id)return &command;
 return nullptr;
}
static_assert(lookup(106)->method=="SetProtocol");
static_assert(lookup(106)->arguments=="(u32 protocol)");
static_assert(lookup(200)->method=="OpenAccessPoint");
static_assert(lookup(300)->method=="OpenStation");
int main(){
 using ams::mitm::ldn::ICommunicationService;uint64_t pid=1;int handle=-1;
 {
  ICommunicationService service;assert(service.AttachStateChangeEvent({&handle})!=0&&handle==-1);
  fail_event=true;assert(service.Initialize(pid)==7&&!live_events);fail_event=false;
  fail_initialize=true;assert(service.Initialize(pid)==9&&live_events==1);fail_initialize=false;
  assert(service.Finalize()==0&&!live_events);
  assert(service.Initialize(pid)==0&&service.AttachStateChangeEvent({&handle})==0&&handle==17);
  while(!signals)std::this_thread::yield();
 }
 assert(!live_events);
 for(int i=0;i<20;i++){
  ICommunicationService service;assert(service.Initialize(pid)==0);
  std::thread attach([&]{for(int j=0;j<100;j++){int h=-1;auto rc=service.AttachStateChangeEvent({&h});assert(rc!=0||h==17);}});
  assert(service.Finalize()==0);attach.join();assert(!live_events);
  assert(service.Finalize()==0&&service.AttachStateChangeEvent({&handle})!=0);
 }
 puts("PASS actual LDN event API: early failures, callbacks during finalization, repeated finalize, invalid attach and destructor ordering");
 // Check setup in either order and continue on the same service object.
 for(bool host:{true,false}){
  ICommunicationService service;
  assert(service.SetProtocol(1)==0);
  assert((host?service.OpenAccessPoint():service.OpenStation())!=0);
  assert(service.Initialize(pid)==0);
  for(u32 protocol:{0u,1u,3u,0xffffffffu})assert(service.SetProtocol(protocol)==0);
  assert((host?service.OpenAccessPoint():service.OpenStation())==0);
 }
 puts("PASS game-facing LDN command 106 before initialization, hosting, and joining");
}
'''
code=code.replace('/* DESTRUCTOR */',extract.function(header,'~ICommunicationService()')).replace('/* COMMANDS */',commands)
code=code.replace('/* FUNCTIONS */','\n'.join(extract.function(source,signature) for signature in [
 'Result ICommunicationService::Initialize(', 'Result ICommunicationService::Finalize(',
 'Result ICommunicationService::AttachStateChangeEvent(', 'void ICommunicationService::onEventFired(',
 'Result ICommunicationService::SetProtocol(', 'Result ICommunicationService::OpenAccessPoint(',
 'Result ICommunicationService::OpenStation(']))
with tempfile.TemporaryDirectory(prefix='native-event-') as tmp:
 cpp=Path(tmp)/'test.cpp';cpp.write_text(code);binary=Path(tmp)/'test'
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-pthread',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=10)
