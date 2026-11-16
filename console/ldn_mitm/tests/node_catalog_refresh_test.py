#!/usr/bin/env python3
"""Exercise actual asynchronous catalog refresh semantics using a simulated directory."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
src=(root/'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
refresh=extract.function(src,'bool RefreshDynamicNodes() {')
get=extract.function(src,'bool GetDynamicNodeCatalog(NodeDynamicCatalog *out) {')
code=r'''
#include "node_catalog.h"
#include "node_lobby.h"
#include "custom_nodes.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <arpa/inet.h>
#include <unistd.h>
namespace custom_nodes=ams::mitm::custom_nodes;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#define R_SUCCEEDED(r) ((r)==0)
#define R_FAILED(r) ((r)!=0)
static std::mutex g_catalog_lock,g_lock,g_catalog_refresh_lock;
static NodeDynamicCatalog g_dynamic_nodes{};static NodeDynamicEntry g_selected_node{};
static std::atomic<bool> g_preference_loaded{true},g_direct_session{false};
static sockaddr_in g_node_endpoints[NODE_TOTAL_CAPACITY]{};
static u32 g_host_node=13,g_selected_status=NodeHostReady;static u64 g_selection_until_ms=1000;
struct Sel{u64 selection_epoch=0;}g_selection;
enum class RelayState{Idle,InRoom,Connecting};static RelayState g_state=RelayState::Idle;
struct DiscoveryRoute{};static bool online=true;static u8 pin[4]={8,8,8,8};static unsigned requests=0;
static u64 SelectionNowMs(){return 100;}
static void ClearSelectionLocked(){g_selection={};}
static size_t NodeSlot(u32 id){return id<NODE_SHARED_CAPACITY?id:id>=NODE_LOCAL_BASE&&id<NODE_LOCAL_BASE+NODE_LOCAL_CAPACITY?NODE_SHARED_CAPACITY+(id-NODE_LOCAL_BASE):NODE_TOTAL_CAPACITY;}
static bool CaptureAppointmentRoute(DiscoveryRoute*){return online;}
static bool ResolveDiscoveryRoute(const DiscoveryRoute&,sockaddr_in*,char*){return online;}
static int ConnectServer(const sockaddr_in&,const char**,int,const char*){return dup(STDOUT_FILENO);}
static bool QueryCapabilities(int,u32*c,int,u32*){*c=NODE_DYNAMIC_CAPABILITY;return online;}
static bool SendFrameTo(int,u8,const void*,size_t,const void*,size_t){return true;}
static ssize_t ReadListFrame(int,u8*t,u8*out,size_t n,int,u32*){
 assert(n>=232);memset(out,0,232);memcpy(out,LanywhereNodeCatalogHash,32);out[32]=1;out[37]=0x20;
 out[40]=13;out[44]=2;out[48]=0xbb;out[49]=0x2c;out[50]=1;
 strcpy((char*)out+52,"shared.example");strcpy((char*)out+180,"Shared");memcpy(out+228,pin,4);
 *t=NODE_DYNAMIC_CATALOG_RESPONSE;return 232;
}
static bool local_ok=true;static bool ReadLocalNodes(char*,size_t,size_t*n){*n=0;return local_ok;}
static void RequestCatalogRefresh(){requests++;}
''' + refresh + '\n' + get + r'''
static void seed(){
 g_dynamic_nodes={};g_dynamic_nodes.version=1;g_dynamic_nodes.count=1;auto &e=g_dynamic_nodes.nodes[0];
 e.id=13;e.protocol=2;e.flags=1;e.port=11451;strcpy(e.host,"shared.example");strcpy(e.name,"Shared");
 e.ipv4[0]=8;e.ipv4[1]=8;e.ipv4[2]=8;e.ipv4[3]=8;g_dynamic_nodes.enabled=g_dynamic_nodes.available=1u<<13;
 g_selected_node=e;g_host_node=13;g_selected_status=NodeHostReady;g_node_endpoints[13]={};
 memcpy(&g_node_endpoints[13].sin_addr.s_addr,e.ipv4,4);g_node_endpoints[13].sin_port=htons(e.port);
 g_state=RelayState::Idle;g_selection={};g_direct_session=false;online=true;local_ok=true;
}
int main(){
 seed();NodeDynamicCatalog snapshot{};assert(GetDynamicNodeCatalog(&snapshot)&&requests==1&&snapshot.count==1);
 pin[3]=9;assert(RefreshDynamicNodes());assert(g_selected_status==NodeHostUnavailable&&!g_node_endpoints[13].sin_addr.s_addr);
 assert(g_dynamic_nodes.nodes[0].ipv4[3]==9);
 seed();g_direct_session=true;pin[3]=9;assert(RefreshDynamicNodes());assert(g_selected_status==NodeHostReady);
 assert(g_dynamic_nodes.nodes[0].ipv4[3]==8&&((u8*)&g_node_endpoints[13].sin_addr.s_addr)[3]==8);
 seed();online=false;assert(RefreshDynamicNodes());assert(g_selected_status==NodeHostUnavailable&&!g_node_endpoints[13].sin_addr.s_addr);
 assert(!g_dynamic_nodes.available&&g_dynamic_nodes.nodes[0].ipv4[0]==0);
 online=true;assert(RefreshDynamicNodes());assert(g_dynamic_nodes.nodes[0].ipv4[3]==9);
 seed();local_ok=false;assert(RefreshDynamicNodes());assert((g_dynamic_nodes.enabled&NODE_LOCAL_CONFIG_INVALID)&&g_dynamic_nodes.count==1&&g_dynamic_nodes.nodes[0].id==13);
 local_ok=true;assert(RefreshDynamicNodes());assert(!(g_dynamic_nodes.enabled&NODE_LOCAL_CONFIG_INVALID));
 seed();online=false;local_ok=false;assert(RefreshDynamicNodes());assert(g_dynamic_nodes.enabled&NODE_LOCAL_CONFIG_INVALID);
 local_ok=true;assert(RefreshDynamicNodes());assert(!(g_dynamic_nodes.enabled&NODE_LOCAL_CONFIG_INVALID));
 seed();g_dynamic_nodes.nodes[1]=g_dynamic_nodes.nodes[0];g_dynamic_nodes.nodes[1].id=NODE_LOCAL_BASE;g_dynamic_nodes.count=2;g_dynamic_nodes.enabled|=1u<<NODE_SHARED_CAPACITY;
 g_selected_node=g_dynamic_nodes.nodes[1];g_host_node=NODE_LOCAL_BASE;g_direct_session=true;local_ok=false;
 assert(RefreshDynamicNodes());assert(g_direct_session&&g_host_node==NODE_LOCAL_BASE&&g_dynamic_nodes.count==1&&(g_dynamic_nodes.enabled&NODE_LOCAL_CONFIG_INVALID));
 assert(!(g_dynamic_nodes.enabled&(1u<<NODE_SHARED_CAPACITY)));
 puts("PASS cached nonblocking IPC, DNS A-to-B revalidation, active-room pin and offline refresh");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-catalog-refresh-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(root/'common'),'-I'+str(root/'console/ldn_mitm/source'),str(p/'test.cpp'),str(root/'console/ldn_mitm/source/custom_nodes.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
