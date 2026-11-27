#!/usr/bin/env python3
"""Actual native control queues and LDN workers, with only the network boundary mocked."""
import ast
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / 'console/ldn_mitm/source'
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('discovery_route_test.py'))
extract = importlib.util.module_from_spec(spec); spec.loader.exec_module(extract)
source = (NATIVE/'lan_discovery.cpp').read_text()
relay = (NATIVE/'lanywhere_relay.cpp').read_text()
direct = (NATIVE/'lanplay_direct.cpp').read_text()
fixture_tree = ast.parse(Path(__file__).with_name('native_state_test.py').read_text())
fixture = next(node.value.value for node in fixture_tree.body if isinstance(node, ast.Assign)
               and any(isinstance(name, ast.Name) and name.id == 'code' for name in node.targets))
fixture = fixture.split('int main(){', 1)[0]
fixture = fixture.replace('#include <algorithm>', '#include <algorithm>\n#include <atomic>\n#include <thread>')
queue_method = extract.function(direct, '    void Queue(uint8_t from,')
direct_queue = '''
namespace ams::mitm::lanplay {
struct Control { uint8_t from=0,kind=0;uint16_t size=0;uint8_t bytes[0x500]{}; };
struct Impl {
 std::mutex mutex;std::array<Control,24>controls{};size_t control_head=0,control_count=0;
 /* QUEUE */
};
static Impl impl;
Impl *Peek(){return &impl;}
/* TAKE */
}
'''.replace('/* QUEUE */', queue_method).replace('/* TAKE */', extract.function(direct, 'ssize_t TakeControl('))
fixture = fixture.replace('namespace ams::mitm::lanywhere {', direct_queue+'\nnamespace ams::mitm::lanywhere {')
queue_source = r'''
static std::atomic<bool> g_direct_session{false};
constexpr size_t ControlQueueSize=16;
struct Control {u8 from;LdnMsg msg;u16 size;u8 data[MaxControl];};
static Control g_controls[ControlQueueSize]{};
static size_t g_control_head=0,g_control_count=0;
static std::mutex g_queue_lock;
/* ENQUEUE */
/* TAKE */
static unsigned sent=0;static bool refill=false;
void Queue(u8 from,LdnMsg kind,const void* data,size_t size){
 if(g_direct_session){auto*p=lanplay::Peek();std::scoped_lock lock(p->mutex);p->Queue(from,static_cast<u8>(kind),data,size);}
 else EnqueueControl(from,kind,static_cast<const u8*>(data),size);
}
size_t Pending(){return g_direct_session?lanplay::Peek()->control_count:g_control_count;}
void Reset(bool direct){
 g_direct_session=direct;g_control_head=13;g_control_count=0;
 lanplay::Peek()->control_head=21;lanplay::Peek()->control_count=0;
 state=RelayState::InRoom;sent=0;refill=false;
}
'''.replace('/* ENQUEUE */', extract.function(relay, 'void EnqueueControl(')).replace('/* TAKE */', extract.function(relay, 'ssize_t TakeControl('))
fixture=fixture.replace('ssize_t TakeControl(u8*,LdnMsg*,void*,size_t,ControlSelector=nullptr,const void* =nullptr){return -1;}',queue_source)
fixture=fixture.replace('bool SendControl(u8,LdnMsg,const void*,size_t){return true;}', '''bool SendControl(u8,LdnMsg,const void*,size_t){
 ++sent;if(refill)Queue(1,LdnMsg::Scan,nullptr,0);return true;
}''').replace('int MemberOfIp(u32){return 0;}', 'int MemberOfIp(u32 ip){return static_cast<int>(ip);}')
fixture=fixture.replace('int stationCount(){return 0;}void updateNodes(){}void onSyncNetwork(NetworkInfo*){}void onDisconnectFromHost(){}', '''int stationCount(){int result=0;for(const auto& s:stations)result+=s.status==NodeStatus::Connected;return result;}
 void updateNodes(){notifications++;}void onSyncNetwork(NetworkInfo*);void onDisconnectFromHost();''')
selector=source[source.index('    struct ControlRequest {'):source.index('    // LANywhere: con una sala activa,',source.index('    struct ControlRequest {'))]
functions=selector+'\n'+'\n'.join(extract.function(source,signature) for signature in [
 'void LANDiscovery::pollRoom(', 'void LANDiscovery::onRoomMessage(',
 'Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork) {',
 'Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork,',
 'void LANDiscovery::onSyncNetwork(', 'void LANDiscovery::onDisconnectFromHost(',
])
fixture=fixture.replace('/* FILTER */',extract.function(source,'static bool MatchesScanFilter(')).replace('/* FUNCTIONS */',functions)
fixture+=r'''
using namespace ams::mitm;using namespace ams::mitm::ldn;
static NetworkInfo Info(u64 game,u8 session){
 NetworkInfo info{};info.networkId.intentId.localCommunicationId=game;
 std::memset(&info.networkId.sessionId,session,sizeof(SessionId));
 info.common.bssid.raw[0]=session;info.ldn.nodeCountMax=8;info.ldn.nodeCount=2;
 info.ldn.nodes[0].localCommunicationVersion=7;return info;
}
static void Visitor(LANDiscovery& v,const NetworkInfo& info){
 v.state=CommState::Station;v.hostMember=0;v.onlineGame=true;
 std::memcpy(v.tcpNetworkId,&info.networkId,sizeof(NetworkId));
}
static void Scanner(LANDiscovery& s,u64 game){
 s.state=CommState::Initialized;s.onlineGame=false;s.autoRoom=false;s.hostMember=-1;
 s.roomScanActive=true;s.activeScanFilter.flag=ScanFilterFlag_LocalCommunicationId;
 s.activeScanFilter.networkId.intentId.localCommunicationId=game;
}
int main(){
 unsigned cases=0;
 for(bool direct:{false,true}){
  for(bool idle_first:{true,false}){
   lanywhere::Reset(direct);const auto info=Info(42,8);
   LANDiscovery idle,visitor;Scanner(idle,99);idle.roomScanActive=false;Visitor(visitor,info);
   lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&info,sizeof(info));
   if(idle_first) { idle.pollRoom(); }
   visitor.pollRoom();
   if(!idle_first) { idle.pollRoom(); }
   assert(visitor.state==CommState::StationConnected&&!lanywhere::Pending());cases++;
  }
  lanywhere::Reset(direct);
  auto info=Info(42,8),other=Info(99,9),wrong_session=Info(42,10);
  LANDiscovery idle,scanner,visitor,host;Scanner(idle,66);idle.roomScanActive=false;
  Scanner(scanner,99);Visitor(visitor,info);
  host.networkInfo=info;for(size_t i=0;i<7;++i)host.stations[i].nodeInfo=&host.networkInfo.ldn.nodes[i+1];
  lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&info,sizeof(info));
  lanywhere::Queue(1,lanywhere::LdnMsg::ScanResp,&other,sizeof(other));
  lanywhere::Queue(1,lanywhere::LdnMsg::ScanResp,&info,sizeof(info));
  lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&wrong_session,sizeof(wrong_session));
  lanywhere::Queue(3,lanywhere::LdnMsg::Sync,&info,sizeof(info));
  NodeInfo node{};node.localCommunicationVersion=7;node.ipv4Address=1;
  lanywhere::Queue(1,lanywhere::LdnMsg::Connect,&node,sizeof(node));
  lanywhere::Queue(1,lanywhere::LdnMsg::Scan,nullptr,0);
  idle.pollRoom();assert(lanywhere::Pending()==7);
  scanner.pollRoom();assert(scanner.udp->scanResults.size()==1&&lanywhere::Pending()==6);
  visitor.pollRoom();assert(visitor.state==CommState::StationConnected&&lanywhere::Pending()==5);
  scanner.activeScanFilter.networkId.intentId.localCommunicationId=42;scanner.pollRoom();
  assert(scanner.udp->scanResults.size()==2&&lanywhere::Pending()==4);
  host.pollRoom();assert(host.stationCount()==1&&lanywhere::sent==1&&lanywhere::Pending()==2);
  lanywhere::Queue(1,lanywhere::LdnMsg::MemberLeft,nullptr,0);host.pollRoom();
  assert(host.stationCount()==0&&lanywhere::Pending()==2);cases++;
  // A disconnect changes the selector immediately. Later SYNC cannot revive it.
  lanywhere::Reset(direct);Visitor(visitor,info);visitor.state=CommState::StationConnected;
  lanywhere::Queue(0,lanywhere::LdnMsg::Disconnect,nullptr,0);
  lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&info,sizeof(info));
  visitor.pollRoom();assert(visitor.state==CommState::Station&&visitor.hostMember==-1&&lanywhere::Pending()==1);
  // Fresh room queues start empty, so an old disconnect cannot hit a new visitor.
  lanywhere::Reset(direct);Visitor(visitor,info);
  lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&info,sizeof(info));visitor.pollRoom();
  assert(visitor.state==CommState::StationConnected);cases++;
  // Wrong-game responses fill the ring, but cannot permanently exclude valid controls.
  lanywhere::Reset(direct);Visitor(visitor,info);
  for(unsigned i=0;i<100;i++)lanywhere::Queue(0,lanywhere::LdnMsg::ScanResp,&other,sizeof(other));
  lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&info,sizeof(info));visitor.pollRoom();
  assert(visitor.state==CommState::StationConnected);cases++;
  // Malformed responses are consumed safely; valid packets remain reachable.
  lanywhere::Reset(direct);Scanner(scanner,42);scanner.udp->scanResults.clear();
  lanywhere::Queue(1,lanywhere::LdnMsg::ScanResp,&info,3);
  lanywhere::Queue(1,lanywhere::LdnMsg::ScanResp,&info,sizeof(info));scanner.pollRoom();
  assert(scanner.udp->scanResults.size()==1&&!lanywhere::Pending());cases++;
  // Continuous peer scans must not hold pollMutex indefinitely.
  lanywhere::Reset(direct);lanywhere::refill=true;
  lanywhere::Queue(1,lanywhere::LdnMsg::Scan,nullptr,0);host.pollRoom();
  assert(lanywhere::sent==24&&lanywhere::Pending()==1);lanywhere::refill=false;cases++;
  // Other worker scheduling cannot steal a visitor's synchronization.
  for(unsigned iteration=0;iteration<100;iteration++){
   lanywhere::Reset(direct);Visitor(visitor,info);Scanner(scanner,99);Scanner(idle,66);idle.roomScanActive=false;
   lanywhere::Queue(1,lanywhere::LdnMsg::ScanResp,&other,sizeof(other));
   lanywhere::Queue(0,lanywhere::LdnMsg::Sync,&info,sizeof(info));
   std::thread a([&]{for(int n=0;n<10;n++)idle.pollRoom();});
   std::thread b([&]{scanner.pollRoom();});std::thread c([&]{visitor.pollRoom();});
   a.join();b.join();c.join();assert(visitor.state==CommState::StationConnected&&!lanywhere::Pending());cases++;
  }
 }
 std::printf("PASS %u actual control delivery cases: both queues, worker ownership, wrapped FIFO, session filters, saturation, cancellation and bounded dispatch\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-delivery-') as temporary:
 temporary=Path(temporary);cpp=temporary/'test.cpp';binary=temporary/'test';cpp.write_text(fixture)
 (temporary/'stratosphere.hpp').write_text('#pragma once\n#include <cstdint>\nusing u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using s64=int64_t;namespace ams::sf{struct LargeData{};struct PrefersPointerTransferMode{};}\n')
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(temporary),'-I'+str(NATIVE),str(cpp),str(NATIVE/'ldn_types.cpp'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=15)
