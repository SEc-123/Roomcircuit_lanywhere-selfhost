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
#include "../../common/node_catalog.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef int PadState;
typedef struct {unsigned count;struct {int x,y;} touches[1];} HidTouchScreenState;
enum {HidNpadButton_A=1,HidNpadButton_B=2,HidNpadButton_X=4,HidNpadButton_Y=8,
HidNpadButton_L=16,HidNpadButton_R=32,HidNpadButton_ZL=64,HidNpadButton_ZR=128,
HidNpadButton_StickR=256,HidNpadButton_AnyUp=512,HidNpadButton_AnyDown=1024};
#define NODE_ROWS 6
#define NODE_ALL_REGIONS 255u
static u64 inputs[8];static size_t frame,frames;static unsigned draws;
static HidTouchScreenState screen;
static int hidGetTouchScreenStates(HidTouchScreenState *out,int n){assert(n==1);*out=screen;return 1;}
static bool appletMainLoop(void){return frame<frames;}
static void padUpdate(PadState *pad){(void)pad;}
static u64 padGetButtonsDown(PadState *pad){(void)pad;return inputs[frame++];}
static void language_input(u64 keys){(void)keys;}
static void svcSleepThread(uint64_t ns){(void)ns;}
static bool ask_game_search(char query[64]){strcpy(query,"Beta");return true;}
static void draw_filters(const GameCatalog *c,const CatalogGameChoice *choices,size_t count,int selected,uint64_t chosen,uint8_t region){(void)c;(void)choices;(void)count;(void)selected;(void)chosen;(void)region;draws++;}
'''+ '\n'.join(routes.function(source, name) for name in (
    'static u64 picker_touch(', 'static bool modal_touch_start(', 'bool nodes_ui_filters(')) + r'''
static bool run(const GameCatalog *catalog,u64 a,u64 b,u64 c,uint64_t *game,uint8_t *region){
 inputs[0]=a;inputs[1]=b;inputs[2]=c;frames=c?3:b?2:1;frame=0;screen.count=0;
 PadState pad=0;return nodes_ui_filters(&pad,catalog,game,region);
}
int main(void){
 unsigned char titles[32]={[0]=7,[8]=1,[16]=9,[24]=7};
 const unsigned char names[]="\0Alpha\0Beta\0";
 GameCatalog catalog={.titles=titles,.title_count=2,.text=names,.text_size=sizeof(names)};
 uint64_t game=9;uint8_t region=255;
 assert(run(&catalog,HidNpadButton_R,HidNpadButton_A,0,&game,&region)&&game==9&&region==0);
 assert(run(&catalog,HidNpadButton_AnyDown,HidNpadButton_A,0,&game,&region)&&game==7);
 assert(run(&catalog,HidNpadButton_X,HidNpadButton_AnyDown,HidNpadButton_A,&game,&region)&&game==9);
 assert(run(&catalog,HidNpadButton_Y,HidNpadButton_A,0,&game,&region)&&game==0);
 game=9;assert(!run(&catalog,HidNpadButton_B,0,0,&game,&region)&&game==9);
 bool held=true;uint32_t rendered[6]={10,2,4,3,5,6},touched=NODE_ID_INVALID;
 screen=(HidTouchScreenState){.count=1,.touches={{100,230}}};
 assert(!picker_touch(rendered,&touched,&held)&&touched==NODE_ID_INVALID);
 screen.count=0;assert(!picker_touch(rendered,&touched,&held));
 screen.count=1;assert(picker_touch(rendered,&touched,&held)==HidNpadButton_A&&touched==10);
 assert(!picker_touch(rendered,&touched,&held));
 assert(draws>=3);
 puts("PASS node input: filter preserves game, exact search choice, clear/cancel, held-touch suppression, rendered node identity");
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
