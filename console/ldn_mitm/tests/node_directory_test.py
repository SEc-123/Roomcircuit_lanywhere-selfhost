#!/usr/bin/env python3
"""Validate the actual native node-directory wire parser and its fallback boundaries."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('discovery_route_test.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
source=(ROOT/'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
header=(ROOT/'console/ldn_mitm/source/lanywhere_relay.hpp').read_text()
def struct(name):
 at=header.index('    struct '+name+' {');return header[at:header.index('};',at)+2]
code=r'''
#include "node_lobby.h"
#include "node_catalog.h"
#include "direct_room_metadata.h"
#include "unified_lobby.h"
#include "list_status.h"
#include "room_game_version.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <unistd.h>
#include <arpa/inet.h>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using s32=int32_t;
constexpr u32 LanPlayNet=0x0a0d0000;constexpr u8 T_Error=0x85;constexpr size_t MaxMembers=8;
namespace util {void Strlcpy(char *out,const char *in,size_t n){if(n)snprintf(out,n,"%s",in);}}
'''+struct('DiscoveryRoute')+'\n'+struct('GroupSummaryV2')+'\n'+''.join(extract.function(source,s)+'\n' for s in ('u32 le32(', 'u64 le64(', 'void put32(', 'void put64('))+r'''
static bool configured=true,current=true,capable=true,annotated=false,wants_annotations=false,bad_hash=false,dynamic=false;
static bool versions=false,wants_versions=false;
struct VerifiedNodeRoom {NodeRoomSelection selection;NodeDynamicEntry entry;u64 expires_at;};
static std::mutex g_verified_rooms_lock;static VerifiedNodeRoom g_verified_rooms[48]{};
static u32 g_verified_snapshot=0;static size_t g_verified_next=0;static constexpr u64 SelectionLifetimeMs=180000;
static u64 SelectionNowMs(){return 1;}
static bool LookupNode(u32 id,NodeDynamicEntry *out){*out={};out->id=id;out->port=11451;return id==13;}
namespace custom_nodes {static bool PublicIpv4(const u8*ip){return ip[0]==8;}}

static std::vector<u8>wire;
static u8 last_command=0, response_override=0;
static int queries=0;
static NodeRoomsRequest received{};
static bool CaptureAppointmentRoute(DiscoveryRoute *out){*out={};return configured;}
static bool ResolveDiscoveryRoute(const DiscoveryRoute&,sockaddr_in *out,char *provider){*out={};strcpy(provider,"self");return true;}
static bool AppointmentBackendCurrent(const DiscoveryRoute&){return current;}
static int ConnectServer(sockaddr_in,const char**,int,const char*,u32*){return dup(STDOUT_FILENO);}
static bool QueryCapabilities(int,u32 *caps,int,u32*){
 *caps=(capable?NODE_LOBBY_CAPABILITY:0)|(annotated?DIRECT_ROOM_CAPABILITY:0)|(dynamic?NODE_DYNAMIC_CAPABILITY:0)|
  (versions?ROOM_GAME_VERSION_CAPABILITY:0);return true;
}
static bool SendFrameTo(int,u8 command,const void *data,size_t size,const void*,size_t){
 last_command=command;
 if(command==((dynamic&&annotated&&versions&&wants_versions)?ROOM_GAME_VERSION_LIST_REQUEST:
  (dynamic&&wants_annotations)?NODE_DYNAMIC_LOBBY_REQUEST:(annotated&&wants_annotations)?DIRECT_ROOM_LIST_REQUEST:NODE_LOBBY_REQUEST)){assert(size==20);memcpy(&received,data,size);queries++;}
 else assert(command==NODE_CATALOG_REQUEST&&size==0);
 return true;
}
static ssize_t ReadListFrame(int,u8 *type,void *out,size_t capacity,int,u32*){
 if(last_command==NODE_CATALOG_REQUEST){assert(capacity>=40);auto*p=static_cast<u8*>(out);memcpy(p,LanywhereNodeCatalogHash,32);if(bad_hash)p[0]^=1;put32(p+32,0x1ffd);put32(p+36,0x1fff);*type=NODE_CATALOG_RESPONSE;return 40;}
 *type=response_override?response_override:last_command==ROOM_GAME_VERSION_LIST_REQUEST?ROOM_GAME_VERSION_LIST_RESPONSE:
  last_command==NODE_DYNAMIC_LOBBY_REQUEST?NODE_DYNAMIC_LOBBY_RESPONSE:last_command==DIRECT_ROOM_LIST_REQUEST?DIRECT_ROOM_LIST_RESPONSE:NODE_LOBBY_RESPONSE;
 assert(capacity>=wire.size());memcpy(out,wire.data(),wire.size());return wire.size();
}
static bool ValidCode(const char*code){return strlen(code)==6;}
static bool CleanDisplayField(const char*field,size_t size,bool){return memchr(field,0,size)!=nullptr;}
static void AddCachedIdentity(GroupSummaryV2&){}
'''+''.join(extract.function(source,s)+'\n' for s in ('static void FindNodeRoomPage(', 'void FindNodeRooms(', 'void FindVersionedNodeRooms('))+r'''
static NodeRoomWire room(){NodeRoomWire row{};row.node_id=2;row.flags=NodeRoomDirect;GroupSummaryV2 group{};
 strcpy(group.code,"ABC234");group.game_id=42;group.players=1;group.max_players=4;strcpy(group.host,"Player");memcpy(row.group,&group,sizeof(group));
 row.locator.host_ip=LanPlayNet|17;row.locator.flavor=NodeFlavorStandard;put64(row.locator.network_id,42);row.locator.bssid[0]=2;row.locator.network_id[16]=3;strcpy(row.locator.code,"ABC234");return row;}
static void response(NodeRoomWire row){wire.assign(16+sizeof(row),0);put32(wire.data(),7);put32(wire.data()+4,NODE_LOBBY_END);wire[8]=1;wire[9]=3;wire[10]=13;memcpy(wire.data()+16,&row,sizeof(row));}
int main(){NodeRoomsRequest req{};req.node_mask=0x1fff;UnifiedPage page{};NodeRoomWire rows[6];unsigned cases=0;
 response(room());FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.count==1&&page.status==ListStatusOk&&rows[0].node_id==2&&rows[0].locator.host_ip==(LanPlayNet|17));assert(received.node_mask==req.node_mask);cases++;
 // Source freshness may exceed host eligibility (original public gateway only).
 assert(page.total==13&&page.available==3);cases++;
 // The same unknown-wire-ID room can carry a trusted display title and access
 // only on the annotated protocol, without changing its locator identity.
 auto enriched=room();GroupSummaryV2 details{};memcpy(&details,enriched.group,sizeof(details));
 details.game_id=UINT64_MAX;details.application_id=42;details.metadata_flags=1;
 details.room_access=RoomAccessShared;strcpy(details.title_en,"Direct Room");
 put64(enriched.locator.network_id,details.game_id);memcpy(enriched.group,&details,sizeof(details));
 response(enriched);FindNodeRooms(req,&page,rows,6,wants_annotations);assert(last_command==NODE_LOBBY_REQUEST&&page.status==ListStatusProtocol);
 annotated=true;response(room());
 FindNodeRooms(req,&page,rows,6,wants_annotations);assert(last_command==NODE_LOBBY_REQUEST&&page.count==1);
 response(enriched);wants_annotations=true;
 FindNodeRooms(req,&page,rows,6,wants_annotations);assert(last_command==DIRECT_ROOM_LIST_REQUEST&&page.count==1);
 GroupSummaryV2 actual{};memcpy(&actual,rows[0].group,sizeof(actual));
 assert(actual.room_access==RoomAccessShared&&actual.game_id==UINT64_MAX&&actual.application_id==42);
 assert(strcmp(actual.title_en,"Direct Room")==0&&memcmp(&rows[0].locator,&enriched.locator,sizeof(enriched.locator))==0);cases++;
 // An annotated capability must match its response and does not bless a gateway.
 response_override=NODE_LOBBY_RESPONSE;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);response_override=0;
 enriched.flags=NodeRoomGateway;response(enriched);FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);cases++;
 annotated=false;response(room());
 FindNodeRooms(req,&page,rows,6,wants_annotations);assert(last_command==NODE_LOBBY_REQUEST&&page.count==1);wants_annotations=false;
 bad_hash=true;int n=queries;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusUpdate&&queries==n);bad_hash=false;
 capable=false;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusUpdate);capable=true;cases++;
 req.node_mask=1;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);req.node_mask=0x1fff;
 req.game_id=43;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);req.game_id=0;cases++;
 auto row=room();row.locator.host_ip=0xc0a80101;response(row);FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);
 row=room();row.locator.code[0]='X';response(row);FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);
 row=room();row.locator.reserved[0]=1;response(row);FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);cases++;
 response(room());req.snapshot=8;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);req.snapshot=0;
 put32(wire.data()+4,2);FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);
 response(room());wire.pop_back();FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);cases++;
 wire.assign(16,0);put32(wire.data()+4,NODE_LOBBY_END);wire[11]=1;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==UnifiedListStale);cases++;
 // Shared custom room retains the specific address validated by the directory.
 dynamic=true;annotated=true;wants_annotations=true;req.node_mask=0x3fff;
 auto custom=room();custom.node_id=13;custom.locator.reserved[0]=8;custom.locator.reserved[1]=8;custom.locator.reserved[2]=8;custom.locator.reserved[3]=8;
 response(custom);wire[10]=14;FindNodeRooms(req,&page,rows,6,wants_annotations);
 assert(last_command==NODE_DYNAMIC_LOBBY_REQUEST&&page.count==1&&g_verified_rooms[0].selection.node_id==13);
 assert(g_verified_rooms[0].selection.locator.reserved[3]==8&&g_verified_rooms[0].entry.port==11451);
 VersionedNodeRoomWire extended[6]{};
 versions=true;wants_versions=true;response(custom);wire[10]=14;wire.resize(wire.size()+16);
 std::memcpy(wire.data()+16+408,"3.0.5",5);
 FindVersionedNodeRooms(req,&page,extended,6);
 assert(page.status==ListStatusOk&&page.count==1&&extended[0].room.node_id==13);
 assert(last_command==ROOM_GAME_VERSION_LIST_REQUEST&&!strcmp(extended[0].game_version,"3.0.5"));
 wire.back()='x';FindVersionedNodeRooms(req,&page,extended,6);
 assert(page.status==ListStatusOk&&page.count==1&&!extended[0].game_version[0]);
 response(custom);wire[10]=14;
 FindVersionedNodeRooms(req,&page,extended,6);assert(page.status==ListStatusProtocol);
 versions=false;wants_versions=false;
 FindVersionedNodeRooms(req,&page,extended,6);
 assert(page.status==ListStatusOk&&page.count==1&&last_command==NODE_DYNAMIC_LOBBY_REQUEST&&!extended[0].game_version[0]);cases++;
 custom.locator.reserved[0]=10;response(custom);wire[10]=14;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusProtocol);
 dynamic=false;annotated=false;wants_annotations=false;req.node_mask=0x1fff;cases++;
 response(room());current=false;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusUnavailable);current=true;configured=false;FindNodeRooms(req,&page,rows,6,wants_annotations);assert(page.status==ListStatusConfig);cases++;
 printf("PASS native node-directory wire parser: %u cases; catalog fingerprint, scoped filters, raw identity, pagination and fallback\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='native-node-directory-') as temporary:
 temporary=Path(temporary);file=temporary/'test.cpp';file.write_text(code);binary=temporary/'test'
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'common'),str(file),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
