#!/usr/bin/env python3
"""Exercise the actual active-route IPC view with controlled session state."""
from pathlib import Path
import importlib.util, os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
src=(root/'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
fn=extract.function(src,'NodeActiveRouteInfo GetActiveNodeRoute() {')
code=r'''
#include "node_catalog.h"
#include "node_lobby.h"
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <arpa/inet.h>
using u32=uint32_t;using u64=uint64_t;
namespace util {static void Strlcpy(char*out,const char*in,size_t n){snprintf(out,n,"%s",in);}}
namespace lanplay {struct Snapshot {bool active=false;u64 epoch=0;};static Snapshot state;static Snapshot State(){return state;}}
struct DiscoveryRoute {u32 ipv4=0;uint16_t port=0;u32 node_id=0;u64 selection_epoch=0;};
struct GameRoute {DiscoveryRoute discovery{};u64 room_epoch=0;};
static std::mutex g_lock;static std::atomic<bool>g_direct_session{false};static GameRoute g_direct_room{};
enum class RelayState{Idle,Connecting,InRoom};static RelayState g_state=RelayState::Idle;
static bool g_recovering=false;static sockaddr_in g_server{};static char g_room_provider[33]="self";
static const char *REGION_PROVIDER_COMMUNITY="community";
static DiscoveryRoute g_selection{};static u64 g_selection_until_ms=0;
static u64 SelectionNowMs(){return 100;}
static bool LoadServer(sockaddr_in*out){*out={};out->sin_addr.s_addr=htonl(0x08080808);out->sin_port=htons(11460);return true;}
static bool LookupNode(u32 id,NodeDynamicEntry*out){if(id>=NODE_SHARED_CAPACITY)return false;*out={};out->id=id;out->protocol=2;out->port=11451;snprintf(out->name,sizeof(out->name),"Node%u",id);return true;}
''' + fn + r'''
int main(){
 auto x=GetActiveNodeRoute();assert(x.status==NodeRouteNone&&x.node_id==NODE_ID_INVALID);
 g_selection.selection_epoch=5;g_selection.node_id=13;g_selection_until_ms=200;
 x=GetActiveNodeRoute();assert(x.status==NodeRouteSelected&&x.node_id==13&&!strcmp(x.name,"Node13"));
 g_selection_until_ms=90;x=GetActiveNodeRoute();assert(x.status==NodeRouteNone);
 g_direct_session=true;g_direct_room.discovery.node_id=13;g_direct_room.room_epoch=9;lanplay::state={true,9};
 x=GetActiveNodeRoute();assert(x.status==NodeRouteConnected&&x.node_id==13);
 lanplay::state.active=false;x=GetActiveNodeRoute();assert(x.status==NodeRouteUnavailable&&x.node_id==13);
 g_direct_session=false;g_state=RelayState::InRoom;g_server.sin_addr.s_addr=htonl(0x08080808);g_server.sin_port=htons(11460);
 x=GetActiveNodeRoute();assert(x.status==NodeRouteConnected&&x.node_id==0&&!strcmp(x.name,"Main server"));
 g_recovering=true;x=GetActiveNodeRoute();assert(x.status==NodeRouteRecovering);
 g_recovering=false;g_server.sin_addr.s_addr=htonl(0x01010101);x=GetActiveNodeRoute();assert(x.node_id==NODE_ID_INVALID&&!strcmp(x.name,"Unknown node"));
 puts("PASS active versus saved route, disconnect, recovery, self endpoint and expired selection");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-active-route-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'common'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
