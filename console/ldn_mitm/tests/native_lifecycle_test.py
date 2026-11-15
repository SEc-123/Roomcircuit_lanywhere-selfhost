#!/usr/bin/env python3
"""Execute production initialization/finalization through faulting service boundaries."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('discovery_route_test.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
source = (ROOT / 'console/ldn_mitm/source/lan_discovery.cpp').read_text()
protocol = (ROOT / 'console/ldn_mitm/source/ldn_types.cpp').read_text()
header = (ROOT / 'console/ldn_mitm/source/lan_discovery.hpp').read_text()
assert 'nifmSetNetworkProfile(' not in source and 'originalMtu' not in source
assert 'this->stop = false' not in extract.function(source, 'void LANDiscovery::worker(')
assert 'std::scoped_lock lock(this->pollMutex)' in extract.function(source, 'Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork,')
assert 'this->roomScanActive && this->udp->scanResults.size() < MaxScanResults' in source
assert 'this->scanResults.size() >= LANDiscovery::MaxScanResults' in source
harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <cstdio>
using Result=int;using u8=uint8_t;
#define R_FAILED(v) ((v)!=0)
#define R_TRY(v) do{auto rc=(v);if(rc)return rc;}while(0)
#define MAKERESULT(m,d) ((m)*1000+(d))
#define LogFormat(...) ((void)0)
template<class F>struct ScopeExit {F function;~ScopeExit(){function();}};
struct ExitMaker{};template<class F>ScopeExit<F>operator+(ExitMaker,F f){return{f};}
#define ON_SCOPE_EXIT auto scoped_exit=ExitMaker{}+[&]()
static int fault,live_requests,created,closed,cancelled,sockets,threads;
static std::atomic<bool>permit_thread{true};
static std::atomic<unsigned>loops{0};
struct TimeSpan {long long ns;static TimeSpan FromSeconds(int n){return {n*1000000000LL};}};
bool operator>=(TimeSpan a,TimeSpan b){return a.ns>=b.ns;}
static std::atomic<long long>clock_ns{0};
namespace os {
using Mutex=std::recursive_mutex;
constexpr size_t ThreadStackAlignment=4096;
struct Tick {long long ns;TimeSpan ToTimeSpan()const{return{ns};}};
Tick GetSystemTick(){return{clock_ns.load()};}Tick operator-(Tick a,Tick b){return{a.ns-b.ns};}
struct ThreadType {std::thread thread;void(*function)(void*);void*argument;};
Result CreateThread(ThreadType *t,void(*f)(void*),void*a,void*,size_t,int,int){
 if(fault==5)return 5;
 t->function=f;t->argument=a;threads++;return 0;
}
void StartThread(ThreadType*t){t->thread=std::thread([t]{while(!permit_thread.load())std::this_thread::yield();t->function(t->argument);});}
void WaitThread(ThreadType*t){t->thread.join();}void DestroyThread(ThreadType*){threads--;}
}
namespace util {uintptr_t AlignUp(uintptr_t value,size_t alignment){return(value+alignment-1)&~(alignment-1);}}
void svcSleepThread(long long ns){clock_ns+=ns;std::this_thread::yield();}
struct NifmRequest {bool live=false;};
enum NifmRequestState {NifmRequestState_Available,NifmRequestState_OnHold};
Result nifmCreateRequest(NifmRequest*r,bool){if(fault==1)return 1;r->live=true;live_requests++;created++;return 0;}
Result nifmSetLocalNetworkMode(NifmRequest*r,bool){assert(r->live);return fault==2?2:0;}
Result nifmRequestSubmit(NifmRequest*r){assert(r->live);return fault==3?3:0;}
Result nifmGetRequestState(NifmRequest*r,NifmRequestState*s){assert(r->live);*s=fault==6?NifmRequestState_OnHold:NifmRequestState_Available;return fault==7?7:0;}
Result nifmGetResult(NifmRequest*r){assert(r->live);return fault==8?8:0;}
Result nifmRequestCancel(NifmRequest*r){assert(r->live);cancelled++;return 0;}
void nifmRequestClose(NifmRequest*r){assert(r->live);r->live=false;closed++;live_requests--;}
namespace tcpstream {struct Proxy{void EndBinding(uint64_t){}};Proxy&Global(){static Proxy p;return p;}}
namespace lanywhere {void PollMatch(){}}
namespace ams::mitm::ldn {
constexpr int ModuleID=253,StackSize=0x8000;
enum class CommState{None,Initialized};enum class DisconnectReason{None};
struct Socket{Socket(){sockets++;}~Socket(){sockets--;}};
struct NodeInfo{};struct Network{struct{NodeInfo nodes[8];}ldn;};
struct Station {NodeInfo *nodeInfo=nullptr;int nodeId=1;void reset(){}};
class LANDiscovery {public:
 using LanEventFunc=std::function<void()>;
 static inline const LanEventFunc EmptyFunc=[]{};
 std::recursive_mutex lifecycleMutex,pollMutex,scanResultsMutex;
 std::atomic<bool>initialized{false},stop{false};
 bool threadCreated=false,requestCreated=false,physicalScanActive=false,roomScanActive=false,haveScanRoute=false;
 size_t publicGroupCount=0;uint64_t tcpBindingId=1;
 NifmRequest request{};os::ThreadType workerThread{};
 std::unique_ptr<Socket>udp,tcp;
 std::unique_ptr<u8[]>stack{new u8[StackSize+os::ThreadStackAlignment]};
 Station stations[7];Network networkInfo{};LanEventFunc lanEvent=EmptyFunc;
 DisconnectReason disconnect_reason=DisconnectReason::None;CommState state=CommState::None;
 static void Worker(void*p){static_cast<LANDiscovery*>(p)->worker();}
 Result initialize(LanEventFunc,bool listening=true);Result finalize();Result restoreNetworkResources();void worker();
 Result initUdp(bool){if(fault==4)return 4;udp=std::make_unique<Socket>();return 0;}
 void setState(CommState v){std::scoped_lock l(pollMutex);state=v;if(lanEvent)lanEvent();}
 void roomCloseStations(){}void leaveAutoRoom(){}void resetStations(){}
 int loopPoll(){loops++;return 0;}
 ~LANDiscovery(){finalize();}
};
/* FUNCTIONS */
}
int main(){
 using namespace ams::mitm::ldn;
 {LANDiscovery game;assert(game.finalize()==0&&game.state==CommState::None);}
 for(int failure=1;failure<=8;failure++){
  fault=failure;clock_ns=0;LANDiscovery game;unsigned signals=0;
  assert(game.initialize([&]{signals++;})!=0);assert(game.state==CommState::None);
  assert(!game.initialized&&!game.requestCreated&&!game.threadCreated&&!game.udp&&live_requests==0&&sockets==0);
  assert(game.finalize()==0);fault=0;assert(game.initialize([&]{signals++;})==0);
  assert(game.finalize()==0&&signals>=2&&live_requests==0&&sockets==0&&threads==0);
 }
 // Worker may be scheduled only after Finalize has already requested stop.
 permit_thread=false;fault=0;LANDiscovery delayed;loops=0;
 assert(delayed.initialize([]{})==0);
 std::thread exit([&]{assert(delayed.finalize()==0);});
 while(!delayed.stop.load())std::this_thread::yield();
 permit_thread=true;exit.join();
 assert(loops==0&&live_requests==0&&sockets==0&&threads==0);
 assert(created==closed&&created==cancelled);
 puts("PASS actual LDN initialize/finalize: all service failures unwind, request handles close, bounded hold and delayed-start stop survives");
}
'''
functions='\n'.join(extract.function(source,s) for s in ['Result LANDiscovery::restoreNetworkResources(', 'Result LANDiscovery::finalize(', 'Result LANDiscovery::initialize(', 'void LANDiscovery::worker('])
with tempfile.TemporaryDirectory(prefix='native-lifecycle-') as tmp:
    cpp=Path(tmp)/'test.cpp';cpp.write_text(harness.replace('/* FUNCTIONS */',functions));binary=Path(tmp)/'test'
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
