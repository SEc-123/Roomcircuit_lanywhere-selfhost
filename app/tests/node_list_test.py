#!/usr/bin/env python3
"""Run actual node-directory IPC parser on typed current and malformed responses."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('routes',root/'app/tests/input_routing_test.py');routes=importlib.util.module_from_spec(spec);spec.loader.exec_module(routes)
source=(root/'app/source/main.c').read_text()
code=r'''
#include "lobby_model.h"
#include "../../common/direct_room_metadata.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned Result;
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define MAKERESULT(m,d) ((m)|((d)<<9))
#define SfBufferAttr_HipcMapAlias 1
#define SfBufferAttr_Out 2
typedef struct {unsigned buffer_attrs[1];struct{void *pointer;size_t size;}buffers[1];} Io;
static UnifiedPage reply;
static LobbyNodeRoom rows[6];
static Result result;
static bool legacy_api;
static bool versioned_api;
static LobbyGameVersion listed_version;
static unsigned last_command;
static bool nodes_ui_valid(uint32_t id) { return id < NODE_CATALOG_COUNT || id == 13; }
static const char *nodes_ui_provider(uint32_t id) { return id == 13 ? "custom-13" : LanywhereNodes[id].provider; }

static Result dispatch(unsigned command, NodeRoomsRequest request, UnifiedPage *out, Io io){
 assert(!request.reserved);
 last_command=command;
 if(command==ROOM_GAME_VERSION_LIST_IPC){
  assert(io.buffers[0].size==sizeof(VersionedNodeRoomWire)*6);
  if(!versioned_api) return MAKERESULT(10,221);
  VersionedNodeRoomWire *versioned=io.buffers[0].pointer;
  for(unsigned i=0;i<6;i++) memcpy(&versioned[i].room,&rows[i],sizeof(rows[i]));
  memcpy(versioned[0].game_version,listed_version,sizeof(listed_version));
  *out=reply;return result;
 }
 assert((command==DIRECT_ROOM_LIST_IPC || command==37) && io.buffers[0].size==sizeof(rows));
 if(legacy_api && command==DIRECT_ROOM_LIST_IPC) return MAKERESULT(10,221);
 memcpy(io.buffers[0].pointer,rows,sizeof(rows));*out=reply;return result;
}
#define serviceDispatchInOut(service,command,request,response,...) dispatch(command,request,&(response),(Io){__VA_ARGS__})
'''+routes.function(source,'static bool unified_ipc_missing(')+'\n'+routes.function(source,'static LobbyPage lanywhere_list_nodes(')+r'''
int main(void){
 LobbyGroup groups[6];LobbySource sources[6];LobbyNodeRoom out[6];LobbyGameVersion versions[6];UnifiedPage page;bool supported=true;
 NodeRoomsRequest request={.node_mask=(1u<<NODE_CATALOG_COUNT)-1};
 reply=(UnifiedPage){.count=1,.next=NODE_LOBBY_END,.snapshot=2,.available=2,.total=13};
 rows[0]=(LobbyNodeRoom){.group={.code="ABC234",.game_id=17,.players=1,.max_players=4},.node_id=2,.flags=NodeRoomDirect};
 memcpy(rows[0].locator.code,"ABC234",7);rows[0].locator.flavor=1;rows[0].locator.host_ip=0x0a0d0101;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && !strcmp(sources[0],"lanplay-nl"));
 assert(last_command==DIRECT_ROOM_LIST_IPC && !versions[0][0]);
 versioned_api=true;
 strcpy(listed_version,"3.0.5");
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && !strcmp(versions[0],"3.0.5"));
 assert(last_command==ROOM_GAME_VERSION_LIST_IPC && !versions[1][0] && out[0].locator.host_ip==rows[0].locator.host_ip);
 memset(listed_version,'2',sizeof(listed_version)); /* No terminator. */
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && !versions[0][0]);
 memset(listed_version,0,sizeof(listed_version));
 strcpy(listed_version,"3.0.5\n");
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && !versions[0][0]);
 versioned_api=false;
 request.node_mask=1;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);
 request.node_mask=(1u<<NODE_CATALOG_COUNT)-1;request.game_id=99;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);
 request.game_id=0;request.snapshot=3;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);request.snapshot=0;
 rows[0].group.players=5;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);rows[0].group.players=1;
 rows[0].locator.code[0]='Z';assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);rows[0].locator.code[0]='A';
 rows[0].locator.reserved[0]=1;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);rows[0].locator.reserved[0]=0;
 rows[0].flags=0;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);rows[0].flags=NodeRoomGateway;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1);
 rows[0].group.room_access=RoomAccessShared;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);
 rows[0].flags=NodeRoomDirect;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && groups[0].room_access==RoomAccessShared);
 request.node_mask=(1u<<NODE_SHARED_CAPACITY)-1;rows[0].node_id=13;reply.total=14;
 rows[0].locator.reserved[0]=8;rows[0].locator.reserved[1]=8;rows[0].locator.reserved[2]=8;rows[0].locator.reserved[3]=8;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && !strcmp(sources[0],"custom-13"));
 rows[0].locator.reserved[4]=1;assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);rows[0].locator.reserved[4]=0;
 memset(rows[0].locator.reserved,0,12);assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);
 rows[0].node_id=2;reply.total=13;request.node_mask=(1u<<NODE_CATALOG_COUNT)-1;
 legacy_api=true;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol && last_command==37);
 rows[0].group.room_access=RoomAccessNone;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).count==1 && last_command==37);
 legacy_api=false; rows[0].group.room_access=RoomAccessShared;
 rows[0].node_id=1;
 assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusProtocol);
 result=MAKERESULT(10,221);assert(lanywhere_list_nodes(request,groups,sources,out,versions,&page,&supported).reserved==ListStatusUpdate&&!supported);
 puts("PASS node directory parser: identity, filter, stale snapshot, counts and explicit legacy fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='node-list-') as td:
    td=Path(td);file=td/'test.c';file.write_text(code);binary=td/'test'
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'app/source'),str(file),str(root/'app/source/lobby_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
