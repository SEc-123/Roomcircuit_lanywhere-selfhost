#!/usr/bin/env python3
"""Actual node selection, friend reports and process socket ownership at the host boundary."""
from pathlib import Path
import importlib.util
import os
import re
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('routes',root/'app/tests/input_routing_test.py');routes=importlib.util.module_from_spec(spec);spec.loader.exec_module(routes)
source=(root/'app/source/nodes_ui.c').read_text()
socket_source=(root/'app/source/app_socket.c').read_text().replace('#include <switch.h>', '')
view=re.search(r'typedef struct \{[^}]*\} NodeView;',source,re.S).group()
code=r'''
#include "node_probe.h"
#include "../../common/node_catalog.h"
#include "game_catalog.h"
#include "relay_config.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NODE_ALL_REGIONS 255u
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define MAKERESULT(module,description) ((module)|((description)<<9))
'''+view+r'''
typedef int Thread;
typedef int Mutex;
typedef unsigned Result;
static NodeView input;
static NodeView view;
static int view_lock;
static bool friend_created;
static bool initialized,socket_ready,nifm_ready;
static Thread friend_thread,probe_threads[3];
static atomic_bool cancel_friend,friend_done,cancel_probe,probe_done[3];
static uint32_t probe_mask;
static unsigned next_probe,probe_create_failures,probe_start_failures;
static uintptr_t first_probe_task,probe_tasks[3];
static uint8_t participant[16]={1};
static NodeFriendRequest friend_request;
static NodeFriendResponse group;
static char group_code[8];
static const char *notice;
static void mutexInit(int *p){(void)p;}
static void mutexLock(int *p){(void)p;}static void mutexUnlock(int *p){(void)p;}
static NodeView snapshot(void){return input;}
typedef struct {uint64_t last_frame_ms,next_check_ms;bool had_focus;} NodeProbeLoop;
static bool probe_created[3],network_change;
static unsigned start_count,invalidations;static bool last_force;static uint32_t last_mask;
enum {AppletFocusState_InFocus=1}; static int focus=1;
static int appletGetFocusState(void){return focus;}
static bool network_changed(void){bool changed=network_change;network_change=false;return changed;}
static void nodes_ui_network_changed(void){
 invalidations++;
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)view.measurements[i].measured_at_ms=0;
}
static void mocked_start_probes(uint32_t mask,bool force){start_count++;last_force=force;last_mask=mask;}

static void friend_worker(void *p){(void)p;}
static void probe_worker(void *p){(void)p;}
static int threadCreate(Thread *t,void(*f)(void*),void *a,void *b,size_t size,int priority,int cpu){
 (void)b;assert(size>=0x10000&&priority==0x2c&&cpu==-2);
 if(f==probe_worker){
    if(probe_create_failures){probe_create_failures--;return 1;}
    probe_tasks[t-probe_threads]=(uintptr_t)a;
 }
 return 0;
}
static int threadStart(Thread *t){
 if(t!=&friend_thread){
    if(probe_start_failures){probe_start_failures--;return 1;}
    if(first_probe_task==UINTPTR_MAX)first_probe_task=probe_tasks[t-probe_threads];
 }
 return 0;
}
static void threadClose(Thread *t){(void)t;}
static int threadWaitForExit(Thread *t){(void)t;return 0;}

enum {Module_Libnx=345,LibnxError_AlreadyInitialized=7,NifmServiceType_User=1};
static bool socket_registered,socket_error;
static unsigned socket_open_calls,socket_close_calls,group_leave_calls;
static Result socketInitializeDefault(void){
 ++socket_open_calls;
 if(socket_registered)return MAKERESULT(Module_Libnx,LibnxError_AlreadyInitialized);
 if(socket_error)return MAKERESULT(Module_Libnx,LibnxError_AlreadyInitialized+1);
 socket_registered=true;return 0;
}
static void socketExit(void){assert(socket_registered);socket_registered=false;++socket_close_calls;}
static Result nifmInitialize(int type){assert(type==NifmServiceType_User);return 0;}
static void nifmExit(void){}
static void randomGet(void *p,size_t n){memset(p,1,n);}
static void cancel_probes(void){memset(probe_created,0,sizeof(probe_created));first_probe_task=UINTPTR_MAX;}
#define node_protocol_exchange mocked_node_protocol_exchange
static bool mocked_node_protocol_exchange(const char *host,uint16_t port,uint8_t command,const void *request,
    size_t request_size,uint8_t expected,void *response,size_t response_size,const atomic_bool *cancel){
 assert(socket_registered);
 assert(!strcmp(host,RELAY_PROFILE_IPV4)&&port==RELAY_PROFILE_PORT);
 assert(command==NODE_FRIEND_REQUEST&&expected==NODE_FRIEND_RESPONSE);
 assert(request_size==sizeof(NodeFriendRequest)&&response_size==sizeof(NodeFriendResponse));
 assert(((const NodeFriendRequest*)request)->operation==NodeFriendLeave&&!atomic_load(cancel));
 memset(response,0,response_size);++group_leave_calls;return true;
}
'''+socket_source+'\n#define start_probes mocked_start_probes\n'+routes.function(source,'static void maintain_probes(')+'\n#undef start_probes\n'+ '\n'.join(routes.function(source, signature) for signature in (
 'static bool ensure_socket(', 'static void start_probes(', 'static bool queue_friend(', 'static void rank_nodes(',
 'void nodes_ui_initialize(', 'void nodes_ui_shutdown('))+'\n'+r'''
int main(void){
 assert(app_socket_acquire()); /* Updater starts first. */
 nodes_ui_initialize();
 assert(socket_open_calls==1&&socket_ready);
 NodeView v={.enabled=(1u<<NODE_CATALOG_COUNT)-1,.catalog_ok=true};
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)v.measurements[i]=(NodeMeasurement){.node_id=i,.state=NodeMeasureReady,.sent=5,.received=5,.kind=NodeProbeUdp,.median_ms=(uint16_t)(50+i),.jitter_ms=2};
 v.measurements[2].median_ms=5;v.measurements[4].median_ms=4;v.measurements[5].median_ms=6;v.measurements[6].median_ms=7;v.measurements[8].median_ms=8;
 uint32_t order[NODE_CATALOG_COUNT];bool recommended[NODE_CATALOG_COUNT];unsigned count;
 rank_nodes(&v,NodeRegionEurope,order,&count,recommended);assert(count==5&&order[0]==4&&recommended[4]&&recommended[5]&&recommended[8]&&!recommended[2]&&!recommended[6]);
 v.measurements[4].received=2;rank_nodes(&v,NodeRegionEurope,order,&count,recommended);assert(!recommended[4]&&recommended[2]);
 rank_nodes(&v,NodeRegionOceania,order,&count,recommended);assert(count==0);
 strcpy(group_code,"ABC234");group.participants=2;
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)group.nodes[i]=(NodeFriendSummary){.node_id=i,.measured_members=2,.reachable_members=2,.minimum_sent=5,.minimum_received=5,.worst_median_ms=300+i,.worst_jitter_ms=5};
 group.nodes[8].worst_median_ms=20;group.nodes[2].reachable_members=1;
 rank_nodes(&v,NodeRegionEurope,order,&count,recommended);assert(order[0]==8&&recommended[8]&&!recommended[2]);
 input=v;uint64_t now=node_probe_now_ms();
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)input.measurements[i].measured_at_ms=now-100;
 input.measurements[0].state=NodeMeasurePending;input.measurements[1].measured_at_ms=now-61000;
 assert(queue_friend(NodeFriendReport,"ABC234"));assert(friend_request.count==11&&friend_request.samples[0].node_id==2&&friend_request.participant[0]==1);
 assert(!queue_friend(NodeFriendPoll,"ABC234"));friend_created=false;
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)input.measurements[i].measured_at_ms=0;
 assert(queue_friend(NodeFriendReport,"ABC234")&&friend_request.count==0);
 NodeProbeLoop loop={.last_frame_ms=node_probe_now_ms(),.had_focus=true};
 input.measurements[2].measured_at_ms=node_probe_now_ms()-61000;
 maintain_probes(&loop,1u<<2);assert(start_count==1&&!last_force&&last_mask==(1u<<2));
 loop.next_check_ms=0;input.measurements[2].measured_at_ms=node_probe_now_ms();network_change=true;
 maintain_probes(&loop,1u<<2);assert(start_count==2&&last_force&&invalidations==1);
 loop.last_frame_ms=node_probe_now_ms()-6000;
 maintain_probes(&loop,1u<<2);assert(start_count==3&&last_force&&invalidations==2);
 focus=0;maintain_probes(&loop,1u<<2);assert(start_count==3);
 focus=1;maintain_probes(&loop,1u<<2);assert(start_count==4&&invalidations==3);
 puts("PASS node choices: region scope, stable quality ranking, distinct hosts, friend worst-path and fresh reports");

 /* Leave the group while the shared socket is still live. */
 nodes_ui_shutdown();
 assert(group_leave_calls==1&&socket_registered&&socket_close_calls==0);
 app_socket_release();
 assert(!socket_registered&&socket_close_calls==1);

 /* Nodes can start first. Either reference may be released first. */
 nodes_ui_initialize();nodes_ui_initialize();
 assert(socket_ready&&socket_open_calls==2);
 assert(app_socket_acquire()&&socket_open_calls==2);
 app_socket_release();
 assert(socket_registered&&socket_close_calls==1);
 nodes_ui_shutdown();nodes_ui_shutdown();
 assert(!socket_registered&&socket_close_calls==2);

 socket_error=true;
 nodes_ui_initialize();
 assert(!socket_ready&&!socket_registered&&socket_open_calls==3);
 now=node_probe_now_ms();
 view=v;
 view.measurements[2].measured_at_ms=now-1;
 view.measurements[3].measured_at_ms=now-61000;
 start_probes((1u<<2)|(1u<<3),true);
 assert(view.probe_start_failed&&view.changed&&socket_open_calls==4);
 assert(view.measurements[2].state==NodeMeasureReady);
 assert(view.measurements[3].state==NodeMeasureFailed&&!view.measurements[3].measured_at_ms);
 assert(view.measurements[4].state==v.measurements[4].state);
 rank_nodes(&view,NODE_ALL_REGIONS,order,&count,recommended);
 assert(!recommended[3]);
 assert(!queue_friend(NodeFriendCreate,NULL)&&!friend_created&&socket_open_calls==5);
 assert(notice&&!strcmp(notice,"Shared test is unavailable. Try again."));
 socket_error=false;
 assert(queue_friend(NodeFriendCreate,NULL)&&friend_created&&socket_open_calls==6);
 probe_create_failures=3;
 start_probes(1u<<3,true);
 assert(view.probe_start_failed&&!probe_created[0]&&!probe_created[1]&&!probe_created[2]);
 probe_start_failures=3;
 start_probes(1u<<3,true);
 assert(view.probe_start_failed&&view.measurements[3].state==NodeMeasureFailed);
 probe_start_failures=1;
 start_probes(1u<<3,true);
 assert(!view.probe_start_failed&&view.measurements[3].state==NodeMeasurePending);
 assert(!probe_created[0]&&probe_created[1]&&first_probe_task==3); /* slot 1 fetches catalog */
 nodes_ui_shutdown();
 assert(!socket_registered&&socket_close_calls==3&&group_leave_calls==1);
 puts("PASS node socket ownership: both start orders, group leave before release, and retry after initialization failure");
 puts("PASS node probe launch: report failed start, preserve fresh results, and retry with the first available worker");
}
'''
with tempfile.TemporaryDirectory(prefix='nodes-model-') as td:
 td=Path(td);file=td/'test.c';file.write_text(code);binary=td/'test'
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'app/source'),str(file),str(root/'app/source/node_probe.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
