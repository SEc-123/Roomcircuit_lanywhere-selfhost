#!/usr/bin/env python3
"""Exercise actual filter modal and node picker touch handling with controller frames."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routes', root / 'app/tests/input_routing_test.py')
routes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routes)
source = (root / 'app/source/nodes_ui.c').read_text()
code = r'''
#include "game_catalog.h"
#include "node_probe.h"
#include "node_selection_trace.h"
#include "../../common/node_catalog.h"
#include "../../common/node_lobby.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef int PadState, Service, NodeProbeLoop;
typedef struct {unsigned count;struct {int x,y;} touches[1];} HidTouchScreenState;
enum {HidNpadButton_A=1,HidNpadButton_B=2,HidNpadButton_X=4,HidNpadButton_Y=8,
HidNpadButton_L=16,HidNpadButton_R=32,HidNpadButton_ZL=64,HidNpadButton_ZR=128,
HidNpadButton_StickR=256,HidNpadButton_AnyUp=512,HidNpadButton_AnyDown=1024,
HidNpadButton_AnyLeft=2048,HidNpadButton_AnyRight=4096};
static const char *keyboard_error;
static u64 inputs[8];static size_t frame,frames;static unsigned draws;
static HidTouchScreenState screen, screen_frames[8];
static bool scripted_touch;
static int hidGetTouchScreenStates(HidTouchScreenState *out,int n){assert(n==1);*out=scripted_touch?screen_frames[frame-1]:screen;return 1;}
static bool appletMainLoop(void){return frame<frames;}
static void padUpdate(PadState *pad){(void)pad;}
static u64 padGetButtonsDown(PadState *pad){(void)pad;return inputs[frame++];}
static void svcSleepThread(uint64_t ns){(void)ns;}
static bool ask_game_search(char query[64]){strcpy(query,"Beta");return true;}
static void draw_filters(const GameCatalog *c,const CatalogGameChoice *choices,size_t count,int selected,uint64_t chosen,uint8_t region,bool focus,bool first,bool next,bool reset){(void)c;(void)choices;(void)count;(void)selected;(void)chosen;(void)region;(void)focus;(void)first;(void)next;(void)reset;draws++;}
''' + '\n'.join(line for line in source.splitlines() if line.startswith('#define NODE_')) + r'''
typedef struct {
 NodeMeasurement measurements[NODE_CATALOG_COUNT];
 uint32_t enabled, available;
 bool catalog_ok, probe_start_failed, changed;
} NodeView;
static NodeView view;
static int view_lock;
static uint32_t dynamic_generation;
static bool initialized=true, friend_created, reverse_after_draw;
static NodeHostInfo current_host_info;static bool current_host_valid;
static NodeDynamicEntry picker_rendered_nodes[NODE_TOTAL_CAPACITY];
static bool picker_rendered_valid[NODE_TOTAL_CAPACITY];
static void refresh_host_info(Service *service){(void)service;}
static bool nodes_ui_refresh_catalog(Service *service){(void)service;return true;}
static uint32_t node_id(uint32_t slot){return slot < NODE_SHARED_CAPACITY ? slot : NODE_LOCAL_BASE + slot - NODE_SHARED_CAPACITY;}
static uint32_t node_slot(uint32_t id){return id < NODE_SHARED_CAPACITY ? id : id >= NODE_LOCAL_BASE && id < NODE_LOCAL_BASE + NODE_LOCAL_CAPACITY ? NODE_SHARED_CAPACITY + id - NODE_LOCAL_BASE : NODE_ID_INVALID;}
static bool nodes_ui_valid(uint32_t id){return id < NODE_CATALOG_COUNT;}
static uint32_t picked, visits;
static uint32_t ipc_result, trace_result;
static NodeSelectionTracePhase trace_phases[4];
static unsigned trace_count;
void node_selection_trace_begin(uint32_t node_id){assert(node_id<NODE_CATALOG_COUNT);trace_count=0;}
void node_selection_trace_phase(NodeSelectionTracePhase phase,uint32_t result){assert(trace_count<4);trace_phases[trace_count++]=phase;trace_result=result;}
static uint64_t next_friend_poll;
static char group_code[8];
static const char *notice;
static void start_probes(uint32_t mask,bool force){assert(mask==((1u<<NODE_CATALOG_COUNT)-1));(void)force;}
static void cancel_probes(void){}
static void collect_probes(void){}
static void maintain_probes(NodeProbeLoop *loop,uint32_t mask){(void)loop;(void)mask;}
static NodeProbeLoop probe_loop_start(void){return 0;}
static bool collect_friend(void){return false;}
uint64_t node_probe_now_ms(void){return 0;}
static void queue_friend(int op,const char *code){(void)op;(void)code;}
static void friend_panel(PadState *pad){(void)pad;}
static NodeView snapshot(void){return view;}
static void mutexLock(int *lock){(void)lock;}
static void mutexUnlock(int *lock){(void)lock;}
static int mock_dispatch(Service *svc,int command,const NodeDynamicEntry *identity){(void)svc;assert(command==NODE_SET_HOST_IDENTITY_IPC);assert(trace_count==1&&trace_phases[0]==NodeSelectionBeforeIpc);picked=identity->id;return (int)ipc_result;}
#define serviceDispatch(s,c,...) mock_dispatch((s),(c),&identity)
#define R_SUCCEEDED(result) ((result)==0)
static void rank_nodes(const NodeView *copy,uint8_t region,uint32_t *order,unsigned *count,bool *recommended)
{
 (void)copy;
 *count=0;
 memset(recommended,0,NODE_CATALOG_COUNT*sizeof(*recommended));
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++) {
  const unsigned id=reverse_after_draw && frame>1 ? NODE_CATALOG_COUNT-1-i : i;
  if(region==NODE_ALL_REGIONS||LanywhereNodes[id].region==region)order[(*count)++]=id;
 }
}
static void draw_picker(const NodeView *copy,uint8_t region,const uint32_t *order,unsigned count,
 unsigned selected,uint32_t current,bool region_focus,const bool *recommended)
{
 (void)copy;(void)region;(void)current;(void)region_focus;(void)recommended;
 memset(picker_rendered_valid,0,sizeof(picker_rendered_valid));
 const unsigned first=selected/NODE_ROWS*NODE_ROWS;
 for(unsigned i=first;i<count&&i<first+NODE_ROWS;i++){visits|=1u<<order[i];picker_rendered_valid[order[i]]=true;picker_rendered_nodes[order[i]].id=node_id(order[i]);}
}
''' + '\n'.join(routes.function(source, name) for name in (
    'static uint8_t region_for_row(', 'static unsigned region_row(', 'static bool region_header_touched(', 'static u64 picker_touch(',
    'static bool modal_touch_start(', 'bool nodes_ui_filters(', 'static unsigned picker_index(', 'bool nodes_ui_pick(')) + r'''
static bool run(const GameCatalog *catalog,u64 a,u64 b,u64 c,uint64_t *game,uint8_t *region){
 inputs[0]=a;inputs[1]=b;inputs[2]=c;frames=c?3:b?2:1;frame=0;screen.count=0;
 PadState pad=0;return nodes_ui_filters(&pad,catalog,game,region);
}
int main(void){
 unsigned char titles[32]={[0]=7,[8]=1,[16]=9,[24]=7};
 const unsigned char names[]="\0Alpha\0Beta\0";
 GameCatalog catalog={.titles=titles,.title_count=2,.text=names,.text_size=sizeof(names)};
 uint64_t game=9;uint8_t region=255;
 assert(run(&catalog,HidNpadButton_R,HidNpadButton_A,0,&game,&region)&&game==9&&region==NodeRegionAsia);
 assert(run(&catalog,HidNpadButton_AnyDown,HidNpadButton_A,0,&game,&region)&&game==7);
 assert(run(&catalog,HidNpadButton_X,HidNpadButton_AnyDown,HidNpadButton_A,&game,&region)&&game==9);
 assert(run(&catalog,HidNpadButton_Y,HidNpadButton_A,0,&game,&region)&&game==0);
 game=9;assert(!run(&catalog,HidNpadButton_B,0,0,&game,&region)&&game==9);
 assert(run(&catalog,HidNpadButton_AnyLeft,HidNpadButton_AnyDown,HidNpadButton_A,&game,&region)&&game==9&&region==NodeRegionEurope);
 assert(region_for_row(NODE_REGION_COUNT-1)==NodeRegionUnknown);
 for(unsigned i=0;i<NODE_REGION_COUNT;i++)assert(region_row(region_for_row(i))==i);
 scripted_touch=true;frame=0;frames=3;game=0;
 inputs[0]=HidNpadButton_AnyLeft;inputs[1]=0;inputs[2]=HidNpadButton_AnyDown|HidNpadButton_A;
 screen_frames[1]=(HidTouchScreenState){.count=1,.touches={{NODE_FILTER_SEARCH_X+8,NODE_PANEL_Y+14}}};
 PadState search_pad=0;
 assert(nodes_ui_filters(&search_pad,&catalog,&game,&region)&&game==9);
 scripted_touch=false;
 bool held=true;uint32_t rendered[6]={10,2,4,3,5,6},touched=NODE_ID_INVALID;
 uint8_t touch_region=NODE_NO_REGION_TOUCHED;
 screen=(HidTouchScreenState){.count=1,.touches={{NODE_TABLE_X+20,NODE_LIST_Y+10}}};
 assert(!picker_touch(rendered,&touched,&touch_region,&held,false,true)&&touched==NODE_ID_INVALID);
 screen.count=0;assert(!picker_touch(rendered,&touched,&touch_region,&held,false,true));
 screen.count=1;assert(picker_touch(rendered,&touched,&touch_region,&held,false,true)==HidNpadButton_A&&touched==10);
 assert(!picker_touch(rendered,&touched,&touch_region,&held,false,true));
 held=false;screen.touches[0].x=84;screen.touches[0].y=NODE_PANEL_Y+14;
 assert(picker_touch(rendered,&touched,&touch_region,&held,false,true)==HidNpadButton_R);
 held=false;screen.touches[0].y=NODE_REGION_Y+NodeRegionEurope*NODE_REGION_HEIGHT+8;
 touched=NODE_ID_INVALID;assert(!picker_touch(rendered,&touched,&touch_region,&held,false,true));
 assert(touch_region==NodeRegionEurope&&touched==NODE_ID_INVALID);
 held=false;screen.touches[0].x=1158;screen.touches[0].y=125;
 assert(picker_touch(rendered,&touched,&touch_region,&held,false,true)==HidNpadButton_ZR);
 held=false;assert(!picker_touch(rendered,&touched,&touch_region,&held,true,false));
 assert(draws>=3);

 PadState pad=0;
 Service service=0;
 uint32_t chosen=NODE_ID_INVALID;
 view.catalog_ok=true;
 view.enabled=((1u<<NODE_CATALOG_COUNT)-1)&~(1u<<8);
 for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)view.measurements[i].state=NodeMeasureReady;
 screen.count=0;frame=0;frames=4;picked=NODE_ID_INVALID;visits=0;
 inputs[0]=0;inputs[1]=HidNpadButton_ZR;inputs[2]=HidNpadButton_ZR;inputs[3]=HidNpadButton_A;
 assert(nodes_ui_pick(&pad,&service,0,&chosen)&&chosen==12&&picked==12);
 assert(trace_count==4&&trace_phases[1]==NodeSelectionAfterIpc&&trace_phases[2]==NodeSelectionBeforeCancelProbes&&trace_phases[3]==NodeSelectionAfterCancelProbes&&trace_result==0);
 assert(visits==((1u<<NODE_CATALOG_COUNT)-1));

 frame=0;frames=6;picked=NODE_ID_INVALID;visits=0;
 inputs[0]=0;inputs[1]=HidNpadButton_ZR;inputs[2]=HidNpadButton_AnyDown;
 inputs[3]=HidNpadButton_AnyDown;inputs[4]=HidNpadButton_A;inputs[5]=HidNpadButton_B;
 assert(!nodes_ui_pick(&pad,&service,0,&chosen)&&picked==NODE_ID_INVALID);

 frame=0;frames=2;picked=NODE_ID_INVALID;reverse_after_draw=true;
 inputs[0]=0;inputs[1]=HidNpadButton_A;
 assert(nodes_ui_pick(&pad,&service,0,&chosen)&&chosen==0&&picked==0);

 frame=0;frames=3;picked=NODE_ID_INVALID;reverse_after_draw=false;ipc_result=0x1159;
 inputs[0]=0;inputs[1]=HidNpadButton_A;inputs[2]=HidNpadButton_B;
 assert(!nodes_ui_pick(&pad,&service,0,&chosen)&&picked==0);
 assert(trace_count==2&&trace_phases[0]==NodeSelectionBeforeIpc&&trace_phases[1]==NodeSelectionAfterIpc&&trace_result==ipc_result);

 puts("PASS node input: exact filters, region focus, held touch, all 13 nodes and disabled/reranked selection");
}
'''
with tempfile.TemporaryDirectory(prefix='node-input-') as temporary:
    temporary = Path(temporary)
    fixture, binary = temporary / 'fixture.c', temporary / 'fixture'
    fixture.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'app/source'), str(fixture), str(root / 'app/source/game_catalog.c'),
                   '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
