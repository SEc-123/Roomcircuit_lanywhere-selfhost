#!/usr/bin/env python3
"""Exercise actual public-node route/host/join entry points with an instrumented transport."""
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
source = (ROOT / 'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
header = (ROOT / 'console/ldn_mitm/source/lanywhere_relay.hpp').read_text()
assert 'g_node_endpoints' not in extract.function(source, 'bool ResolveNodeEndpoint('), 'DNS resolution must not mutate a selected/active route'
def struct(name):
    at = header.index('    struct ' + name + ' {')
    return header[at:header.index('};', at) + 2]
def actual(signature):
    return extract.function(source, signature) + '\n'

code = r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include "regional_relay.h"
#include "unified_lobby.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <arpa/inet.h>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using Result=int;
#define R_RETURN(x) return (x)
#define R_SUCCEED() return 0
constexpr size_t NickLength=32,CodeLength=6;
constexpr u8 Features=3,T_Create=1,T_Join=2;
constexpr u32 LanPlayNet=0x0a0d0000;
namespace util {void Strlcpy(char *out,const char *in,size_t n){if(n)std::snprintf(out,n,"%s",in);}}
namespace os {struct Tick {u64 ms;Tick ToTimeSpan()const{return *this;}u64 GetMilliSeconds()const{return ms;}};Tick GetSystemTick(){return {1000};}}
namespace ldn {struct ScanFilter{u32 flag;struct{struct{u64 localCommunicationId;}intentId;}networkId;};}
namespace tcpstream {constexpr u32 Capability=128,LegacyCapability=256;void ReceiveFrame(u64,const void*,size_t){}}
enum class RelayState {Idle,Connecting,InRoom,Error};
static std::mutex g_lock,g_match_lock,g_room_lifecycle_lock;
static RelayState g_state=RelayState::Idle;
static bool g_public=true,g_recovering=false;
static char g_error[128]{},g_room_provider[33]="self",g_code[8]="ABC234";
static sockaddr_in g_server{};
static std::atomic<u64>g_stream_epoch{1};
static std::atomic<bool>g_stream_ready{false},g_direct_session{false};
static std::atomic<u32>g_net_ip{0};
static u32 g_token=0,g_capabilities=0,g_my_ip=0;
static bool LoadServer(sockaddr_in *server=nullptr,char *provider=nullptr){
 if(provider)util::Strlcpy(provider,"self",33);
 if(server){*server={};server->sin_addr.s_addr=htonl(0x31e993cf);server->sin_port=htons(11460);}return true;
}
static Result ResultRelayFailed(){return 1;}
static Result ResultCommunityUnreachable(){return 2;}
static void SetError(const char *text){util::Strlcpy(g_error,text,sizeof(g_error));g_state=RelayState::Error;}
static void Nickname(char *out,size_t size){util::Strlcpy(out,"Player",size);}
static u64 le64(const u8 *p){u64 v=0;for(int i=0;i<8;i++)v|=u64(p[i])<<(8*i);return v;}
static void put64(u8 *p,u64 v){for(int i=0;i<8;i++)p[i]=u8(v>>(8*i));}
static bool ValidCode(const char *code){return strlen(code)==6;}
''' + struct('DiscoveryRoute') + '\n' + struct('GameRoute') + r'''
static GameRoute g_direct_room{};
namespace lanplay {
struct Endpoint{u32 ipv4;u16 port;};
struct Snapshot{bool active=false,hosting=false;u32 node=0,address=0,sent=0,received=0;u64 epoch=0,game=0;u8 member=0,members=0;char error[64]{};};
static Snapshot state;static Endpoint target{};static bool reachable=true,configured=false,callback_ready=false;static int probes=0,hosts=0,joins=0,scans=0,leaves=0;
static NodeRoomLocator seen_locator{};static bool saw_locator=false;static u64 next_epoch=1;
Snapshot State(){return state;}
static void(*probe_hook)()=nullptr;
bool Probe(Endpoint endpoint){assert(endpoint.ipv4&&endpoint.port);probes++;if(probe_hook)probe_hook();return reachable;}
bool Configure(u32 node,Endpoint endpoint){if(state.active)return false;state.node=node;target=endpoint;configured=true;return true;}
void SetStreamReply(void(*)(u64,const void*,size_t)){if(configured)callback_ready=true;}
bool Host(u64 game){assert(configured&&callback_ready);hosts++;state.active=state.hosting=true;state.game=game;state.address=LanPlayNet|0x1234;state.epoch=(u64(1)<<63)|next_epoch++;return true;}
bool Join(const char *code,u64 game){assert(!strcmp(code,"ABC234"));joins++;return Host(game);}
void Leave(){state.active=false;callback_ready=false;leaves++;}
int Scan(const void*,size_t,const NodeRoomLocator *locator,void(*)(const char*,const void*,void*),void*){scans++;saw_locator=locator!=nullptr;if(locator)seen_locator=*locator;return 1;}
}
static bool InRoom(){return g_direct_session?lanplay::State().active:g_state==RelayState::InRoom;}
static void LeaveCurrentRoom(){if(g_direct_session.exchange(false))lanplay::Leave();g_direct_room={};g_state=RelayState::Idle;}
struct MatchTicket{u64 id=0;};
struct MatchData{bool in_flight=false;MatchTicket ticket;};
static MatchData g_match;static MatchTicket g_release_next;
static bool ArmedLocked(){return false;}
static bool MatchArmed(){return false;}
static void ReleaseTicket(MatchTicket){}
''' + extract.selection_state(source) + r'''
static u32 resolved_extra=0;
static bool ResolveNodeEndpoint(u32 node,sockaddr_in *out){
 if(node>=NODE_CATALOG_COUNT)return false;
 *out={};out->sin_addr.s_addr=htonl(0x08080808+node+resolved_extra);out->sin_port=htons(LanywhereNodes[node].port);
 return true;
}
static bool CommunityReachable(const sockaddr_in&){return lanplay::reachable;}
static bool CaptureAppointmentRoute(DiscoveryRoute *out){*out={};out->ipv4=htonl(0x31e993cf);out->port=htons(11460);return true;}
static int gateway_joins=0;
static Result SelectUnifiedRoom(const UnifiedRoomRequest &room){gateway_joins++;return strcmp(room.provider,"self")&&strcmp(room.provider,REGION_PROVIDER_COMMUNITY);}
static void CancelMatch(){ClearSelectionLocked();}
''' + ''.join(actual(s) for s in (
    'bool CaptureDiscoveryRoute(', 'bool CaptureGameRoute(', 'bool CaptureHostGameRoute(',
    'bool GameRouteStillCurrent(', 'bool DiscoveryRouteStillSelected(const DiscoveryRoute &route) {',
    'Result EnterDirectNode(', 'u32 GetHostNode(', 'Result SetHostNode(', 'Result SelectNodeRoom(',
)) + r'''
static int central_entries=0;
static Result Enter(u8,const u8*,size_t,const sockaddr_in*,u8*,const u8*,const DiscoveryRoute*,GameRoute*){central_entries++;return 0;}
''' + ''.join(actual(s) for s in (
    'Result CreateRoom(', 'Result JoinRoom(', 'void LeaveGameRoom(', 'int FindDirectNodeGroups(', 'bool CaptureTcpTransport(',
)) + r'''
static void found(const char*,const void*,void*){}
int main(){
 unsigned cases=0;
 assert(SetHostNode(2)==0&&GetHostNode()==2);GameRoute host{};
 assert(CaptureHostGameRoute(&host)&&host.discovery.node_id==2);
 assert(host.discovery.ipv4==htonl(0x0808080a)&&!strcmp(host.discovery.provider,"lanplay-nl"));
 GameRoute forged=host;forged.discovery.node_id=3;assert(!GameRouteStillCurrent(forged));
 GameRoute joined{};assert(CreateRoom(42,&host.discovery,&joined)==0);
 assert(lanplay::hosts==1&&central_entries==0&&joined.discovery.node_id==2&&g_net_ip==0x0a0d1234);
 assert(SetHostNode(3)!=0&&GetHostNode()==2);cases++;
 GameRoute captured{};assert(CaptureGameRoute(&captured)&&GameRouteStillCurrent(captured));
 u64 epoch=0;u32 caps=0,ip=0;assert(CaptureTcpTransport(&epoch,&caps,&ip)&&epoch==captured.room_epoch&&caps==(128|256)&&ip==0x0a0d1234);
 GameRoute old=joined;old.discovery.node_id=3;LeaveGameRoom(old);assert(lanplay::state.active);
 old=joined;old.room_epoch++;LeaveGameRoom(old);assert(lanplay::state.active);
 LeaveGameRoom(joined);assert(!g_direct_session&&!lanplay::state.active);cases++;
 // A selected remote room overrides the host preference and preserves all raw identity.
 NodeRoomSelection request{};request.node_id=3;request.game_id=43;request.locator.flavor=NodeFlavorFork;
 request.locator.host_ip=LanPlayNet|0x5678;request.locator.bssid[0]=2;request.locator.network_id[16]=9;
 put64(request.locator.network_id,43);strcpy(request.locator.code,"ABC234");
 assert(SelectNodeRoom(request)==0&&GetHostNode()==2);assert(CaptureGameRoute(&captured)&&captured.discovery.node_id==3);
 assert(FindDirectNodeGroups(captured.discovery,nullptr,found,nullptr)==1&&lanplay::saw_locator);
 assert(!memcmp(&lanplay::seen_locator,&request.locator,sizeof(request.locator))&&lanplay::state.node==3);
 const int attempts=lanplay::joins;
 assert(JoinRoom("ABC234",44,&captured.discovery,nullptr)!=0&&lanplay::joins==attempts);
 assert(JoinRoom("ABC234",43,&captured.discovery,&joined)==0&&lanplay::state.node==3&&central_entries==0);
 assert(lanplay::target.ipv4==htonl(0x0808080b));LeaveGameRoom(joined);cases++;
 request.locator.network_id[16]=0;assert(SelectNodeRoom(request)!=0);
 request.locator.network_id[16]=9;request.locator.host_ip=0xc0a80101;assert(SelectNodeRoom(request)!=0);
 request.locator.host_ip=LanPlayNet|0x5678;request.locator.reserved[0]=1;assert(SelectNodeRoom(request)!=0);cases++;
 // Choosing another node invalidates a prior idle route; a failed probe changes no preference.
 assert(SetHostNode(2)==0&&CaptureHostGameRoute(&host));assert(SetHostNode(3)==0);
 assert(CreateRoom(42,&host.discovery,nullptr)!=0);
 lanplay::reachable=false;assert(SetHostNode(4)==ResultCommunityUnreachable()&&GetHostNode()==3);lanplay::reachable=true;cases++;
 // A failed DNS/probe attempt and a room opened during probing leave its endpoint pinned.
 const auto original=g_node_endpoints[3];resolved_extra=100;
 lanplay::reachable=false;assert(SetHostNode(3)==ResultCommunityUnreachable());
 assert(g_node_endpoints[3].sin_addr.s_addr==original.sin_addr.s_addr);lanplay::reachable=true;
 lanplay::probe_hook=+[]{g_direct_session=true;};
 assert(SetHostNode(3)==ResultRelayFailed()&&g_node_endpoints[3].sin_addr.s_addr==original.sin_addr.s_addr);
 g_direct_session=false;request={};request.node_id=3;request.game_id=43;
 request.locator.flavor=NodeFlavorStandard;request.locator.host_ip=LanPlayNet|0x5678;
 request.locator.bssid[0]=2;request.locator.network_id[16]=9;put64(request.locator.network_id,43);strcpy(request.locator.code,"ABC234");
 assert(SelectNodeRoom(request)==ResultRelayFailed()&&g_node_endpoints[3].sin_addr.s_addr==original.sin_addr.s_addr);
 lanplay::probe_hook=nullptr;g_direct_session=false;resolved_extra=0;cases++;
 // Self/original-public rows keep the established selector, not LAN Play synthetic codes.
 request={};request.node_id=0;request.game_id=42;strcpy(request.locator.code,"ABC234");assert(SelectNodeRoom(request)==0&&gateway_joins==1);
 request.node_id=1;assert(SelectNodeRoom(request)==0&&gateway_joins==2);cases++;
 g_relay_region=RelayRegionEurope;assert(GetHostNode()==NODE_ID_INVALID);cases++;
 printf("PASS native public-node entry points: %u cases; direct route pinning, raw identity, lifecycle, stale routes and legacy selector\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='node-entry-') as temporary:
    temporary=Path(temporary);file=temporary/'test.cpp';file.write_text(code);binary=temporary/'test'
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(ROOT/'common'),str(file),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
print('Actual production entry-point slices; transport is instrumented here, actual wire covered separately.')
