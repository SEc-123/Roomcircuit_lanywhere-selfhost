#!/usr/bin/env python3
"""Exercise actual LDN final transport failure, scan bounds and deterministic outputs."""
from pathlib import Path
import importlib.util,os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
source=(ROOT/'console/ldn_mitm/source/lan_discovery.cpp').read_text()
code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include "ldn_types.hpp"
#include "public_scan_pages.hpp"
using Result=int;
#define MAKERESULT(m,d) ((m)*1000+(d))
void MockLog(const char*,...){}
#define LogFormat MockLog
namespace ams::mitm::lanywhere {
constexpr size_t MaxControl=0x500;enum class RelayState{Idle,Connecting,InRoom,Error};
enum class LdnMsg{Scan,ScanResp,Connect,Sync,Disconnect,MemberLeft};
struct Status{RelayState state;};static RelayState state=RelayState::InRoom;
bool InRoom(){return state==RelayState::InRoom||state==RelayState::Connecting;}
void GetStatus(Status*out){out->state=state;}
ssize_t TakeControl(u8*,LdnMsg*,void*,size_t){return -1;}
void Log(const char*,...){}
bool SendControl(u8,LdnMsg,const void*,size_t){return true;}
int MemberOfIp(u32){return 0;}
}
namespace ams::mitm::ldn {
constexpr int ModuleID=253,LdnModuleId=203,LdnAcceptPolicy_AlwaysReject=1;
/* FILTER */
enum class DisconnectReason {None,SignalLost};
enum class NodeStatus {Disconnected,Connected};
struct LanStation {
 int member=-1;NodeStatus status=NodeStatus::Disconnected;NodeInfo*nodeInfo=nullptr;
 NodeStatus getStatus(){return status;}void reset(){status=NodeStatus::Disconnected;member=-1;}
};
struct Less {bool operator()(const MacAddress&a,const MacAddress&b)const{return std::memcmp(a.raw,b.raw,6)<0;}};
struct Udp {std::map<MacAddress,NetworkInfo,Less>scanResults;};
class LANDiscovery {public:
 static constexpr size_t MaxScanResults=8;
 std::recursive_mutex pollMutex,scanResultsMutex;
 bool initialized=true,onlineGame=true,autoRoom=true,roomScanActive=false;
 u8 controlBuffer[lanywhere::MaxControl]{};int hostMember=0;
 CommState state=CommState::AccessPointCreated;DisconnectReason disconnect_reason=DisconnectReason::None;
 ScanFilter activeScanFilter{};NetworkInfo networkInfo{};std::array<NodeLatestUpdate,8>nodeChanges{};
 LanStation stations[7];std::unique_ptr<Udp>udp{new Udp};unsigned notifications=0,leaves=0,bindings=0;
 void pollRoom();void onRoomMessage(u8,u8,const void*,size_t);
 Result getNetworkInfo(NetworkInfo*);Result getNetworkInfo(NetworkInfo*,NodeLatestUpdate*,int);
 void resetStations(){for(auto&s:stations)s.reset();}
 void leaveAutoRoom(){onlineGame=false;autoRoom=false;leaves++;}
 void onNetworkInfoChanged(){notifications++;}void setState(CommState s){state=s;notifications++;}
 void refreshTcpBinding(){bindings++;}
 int stationCount(){return 0;}void updateNodes(){}void onSyncNetwork(NetworkInfo*){}void onDisconnectFromHost(){}
};
/* FUNCTIONS */
}
int main(){
 using namespace ams::mitm;using namespace ams::mitm::ldn;
 LANDiscovery game;game.networkInfo.ldn.nodeCount=2;for(auto&node:game.networkInfo.ldn.nodes)node.isConnected=1;
 lanywhere::state=lanywhere::RelayState::Connecting;game.pollRoom();assert(game.state==CommState::AccessPointCreated&&!game.leaves);
 lanywhere::state=lanywhere::RelayState::Error;game.pollRoom();assert(game.state==CommState::Error&&game.disconnect_reason==DisconnectReason::SignalLost&&game.leaves==1);
 for(auto&node:game.networkInfo.ldn.nodes)assert(!node.isConnected);
 assert(game.networkInfo.ldn.nodeCount==0);NetworkInfo out{};assert(game.getNetworkInfo(&out)!=0);
 LANDiscovery scanner;NetworkInfo response{};scanner.onlineGame=false;
 scanner.roomScanActive=true;scanner.activeScanFilter.flag=ScanFilterFlag_LocalCommunicationId;
 scanner.activeScanFilter.networkId.intentId.localCommunicationId=42;
 response.networkId.intentId.localCommunicationId=99;
 for(int id=0;id<64;id++){response.common.bssid.raw[0]=id;scanner.onRoomMessage(0,static_cast<u8>(lanywhere::LdnMsg::ScanResp),&response,sizeof(response));}
 assert(scanner.udp->scanResults.empty());response.networkId.intentId.localCommunicationId=42;
 for(int active:{0,1}){
  scanner.roomScanActive=active;
  for(int id=0;id<64;id++){response.common.bssid.raw[0]=id;scanner.onRoomMessage(0,static_cast<u8>(lanywhere::LdnMsg::ScanResp),&response,sizeof(response));}
  assert(scanner.udp->scanResults.size()==(active?8:0));
 }
 scanner.state=CommState::StationConnected;scanner.nodeChanges[0].stateChange=NodeStateChange_Connect;
 std::array<NodeLatestUpdate,8>updates;std::memset(updates.data(),0xaa,sizeof(updates));
 assert(scanner.getNetworkInfo(&out,updates.data(),updates.size())==0&&updates[0].stateChange==NodeStateChange_Connect);
 for(const auto&update:updates)for(auto byte:update._unk)assert(byte==0);
 assert(scanner.nodeChanges[0].stateChange==NodeStateChange_None);
 NetworkConfig config;std::memset(&config,0xaa,sizeof(config));out.ldn.nodeCountMax=8;
 NetworkInfo2NetworkConfig(&out,&config);assert(!config._unk1);for(auto byte:config._unk2)assert(byte==0);
 Ssid a{},b{};a.length=b.length=255;assert(!(a==b));a="Room";b="Room";assert(a==b);
 puts("PASS actual LDN state: final transport loss reaches game, recovery preserves state, unsolicited/bounded scans and zeroed outputs");
}
'''
functions='\n'.join(extract.function(source,s) for s in ['void LANDiscovery::pollRoom(', 'void LANDiscovery::onRoomMessage(', 'Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork) {', 'Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork,'])
with tempfile.TemporaryDirectory(prefix='native-state-') as tmp:
 tmp=Path(tmp);cpp=tmp/'test.cpp';cpp.write_text(code.replace('/* FILTER */',extract.function(source, 'static bool MatchesScanFilter(')).replace('/* FUNCTIONS */',functions));binary=tmp/'test'
 (tmp/'stratosphere.hpp').write_text('#pragma once\n#include <cstdint>\nusing u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using s64=int64_t;namespace ams::sf{struct LargeData{};struct PrefersPointerTransferMode{};}\n')
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(tmp),'-I'+str(ROOT/'console/ldn_mitm/source'),str(cpp),str(ROOT/'console/ldn_mitm/source/ldn_types.cpp'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=10)
