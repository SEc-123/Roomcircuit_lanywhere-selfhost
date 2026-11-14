#!/usr/bin/env python3
"""Actual node recommendation and friend-report construction, without console services."""
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
view=re.search(r'typedef struct \{[^}]*\} NodeView;',source,re.S).group()
code=r'''
#include "node_probe.h"
#include "../../common/node_catalog.h"
#include "game_catalog.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NODE_ALL_REGIONS 255u
#define R_FAILED(x) ((x)!=0)
'''+view+r'''
typedef int Thread;
static NodeView input;
static int view_lock;
static bool friend_created;
static Thread friend_thread;
static atomic_bool cancel_friend,friend_done;
static uint8_t participant[16]={1};
static NodeFriendRequest friend_request;
static NodeFriendResponse group;
static char group_code[8];
static void mutexLock(int *p){(void)p;}static void mutexUnlock(int *p){(void)p;}
static NodeView snapshot(void){return input;}
typedef struct {uint64_t last_frame_ms,next_check_ms;bool had_focus;} NodeProbeLoop;
static bool probe_created[3],network_change;
static unsigned start_count,invalidations;static bool last_force;static uint32_t last_mask;
enum {AppletFocusState_InFocus=1}; static int focus=1;
static int appletGetFocusState(void){return focus;}
static bool network_changed(void){bool changed=network_change;network_change=false;return changed;}
static void nodes_ui_network_changed(void){invalidations++;}
static void start_probes(uint32_t mask,bool force){start_count++;last_force=force;last_mask=mask;}

static void friend_worker(void *p){(void)p;}
static int threadCreate(Thread *t,void(*f)(void*),void *a,void *b,size_t size,int priority,int cpu){(void)t;(void)f;(void)a;(void)b;assert(size>=0x10000&&priority==0x2c&&cpu==-2);return 0;}
static int threadStart(Thread *t){(void)t;return 0;}static void threadClose(Thread *t){(void)t;}
'''+routes.function(source,'static void maintain_probes(')+'\n'+routes.function(source,'static bool queue_friend(')+'\n'+routes.function(source,'static void rank_nodes(')+'\n'+r'''
int main(void){
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
}
'''
with tempfile.TemporaryDirectory(prefix='nodes-model-') as td:
 td=Path(td);file=td/'test.c';file.write_text(code);binary=td/'test'
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'app/source'),str(file),str(root/'app/source/node_probe.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
