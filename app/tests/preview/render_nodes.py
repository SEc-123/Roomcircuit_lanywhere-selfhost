#!/usr/bin/env python3
"""Render actual node selection, shared-test, filter and join screens with declared samples."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
here=Path(__file__).resolve().parent
source=here.parents[1]/'source'
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('routes',here.parent/'input_routing_test.py');routes=importlib.util.module_from_spec(spec);spec.loader.exec_module(routes)
text=(source/'nodes_ui.c').read_text()
functions=['const char *nodes_ui_name(','const char *nodes_ui_region(','static const char *country_name(','static void rank_nodes(','static void measurement_label(','static void begin_screen(','static void draw_picker(','static void draw_friend_panel(','static void draw_room_check(','static void draw_filters(']
code='''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "ui_text.h"
#include "node_probe.h"
#include "game_catalog.h"
#include "../../common/node_catalog.h"
'''+ '\n'.join(line for line in text.splitlines() if line.startswith('#define NODE_'))+'\n'+re.search(r'typedef struct \{[^}]*\} NodeView;',text,re.S).group()+'''
static NodeFriendResponse group;
static char group_code[8];
static const char *notice;
static const char *keyboard_error;
static bool language_save_failed;
'''+ '\n'.join(routes.function(text,x) for x in functions)+r'''
static void capture(const char *dir,const char *kind,int zh){char path[4096];snprintf(path,sizeof(path),"%s/nodes-%s-%s.png",dir,zh?"zh":"en",kind);assert(gfx_save_png(path));}
int main(int argc,char **argv){
 assert(argc==2&&gfx_init());
 for(int zh=0;zh<2;zh++){
  ui_set_language(zh,false);NodeView v={.catalog_ok=true,.enabled=(1u<<NODE_CATALOG_COUNT)-1,.available=(1u<<NODE_CATALOG_COUNT)-1};
  for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)v.measurements[i]=(NodeMeasurement){.node_id=i,.state=NodeMeasureReady,.sent=5,.received=5,.kind=i<2?NodeProbeTcp:NodeProbeUdp,.median_ms=(uint16_t)(40+i*8),.jitter_ms=5};
  uint32_t order[NODE_CATALOG_COUNT];unsigned count;bool recommended[NODE_CATALOG_COUNT];
  rank_nodes(&v,NODE_ALL_REGIONS,order,&count,recommended);draw_picker(&v,NODE_ALL_REGIONS,order,count,0,recommended);capture(argv[1],"all",zh);
  rank_nodes(&v,NodeRegionEurope,order,&count,recommended);draw_picker(&v,NodeRegionEurope,order,count,0,recommended);capture(argv[1],"europe",zh);
  rank_nodes(&v,NodeRegionOceania,order,&count,recommended);draw_picker(&v,NodeRegionOceania,order,count,0,recommended);capture(argv[1],"empty",zh);
  strcpy(group_code,"ABC234");group.participants=group.reported_participants=2;
  for(unsigned i=0;i<NODE_CATALOG_COUNT;i++)group.nodes[i]=(NodeFriendSummary){.node_id=i,.measured_members=2,.reachable_members=2,.minimum_sent=5,.minimum_received=5,.worst_median_ms=(uint32_t)(80+i*8),.worst_jitter_ms=12};
  rank_nodes(&v,NodeRegionEurope,order,&count,recommended);draw_picker(&v,NodeRegionEurope,order,count,0,recommended);capture(argv[1],"shared",zh);
  draw_friend_panel();capture(argv[1],"code",zh);
  v.measurements[1].median_ms=310;v.measurements[1].jitter_ms=91;draw_room_check(&v.measurements[1],1,false,false);capture(argv[1],"weak",zh);
  draw_room_check(&v.measurements[0],0,true,false);capture(argv[1],"gateway",zh);
  CatalogGameChoice games[2]={{17,{.en="Pokémon Violet",.zh="宝可梦 紫",.kind=CatalogCommunication}},{18,{.en="Mario Kart 8 Deluxe",.zh="马力欧卡丁车8 豪华版",.kind=CatalogCommunication}}};
  GameCatalog catalog={0};draw_filters(&catalog,games,2,0,0,NodeRegionEurope);capture(argv[1],"filters",zh);
  group_code[0]=0;memset(&group,0,sizeof(group));
  for(unsigned i=0;i<NODE_CATALOG_COUNT;i++){
   assert(gfx_text_width(22,nodes_ui_name(i)) + gfx_text_width(22,ui_text(" (Recommended for you)")) < 700);
  }
 }
 gfx_exit();puts("Nodes: 16 bilingual previews from actual drawing functions");
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','freetype2','libpng'],text=True))
with tempfile.TemporaryDirectory(prefix='node-preview-') as td:
 td=Path(td);p=td/'preview.c';p.write_text(code);binary=td/'preview'
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(here),'-I'+str(source),str(p),str(source/'gfx.c'),str(source/'ui_text.c'),str(source/'node_probe.c'),str(source/'game_catalog.c'),str(here/'host_switch.c'),'-o',str(binary)]+flags+['-lm'],check=True)
 r=subprocess.run([str(binary),str(out)],capture_output=True,text=True,check=True);print(r.stdout,end='')
(out/'preview-report.json').write_text(json.dumps({'scope':'actual node UI drawings with declared sample state','physicalSwitch':False,'sourceSha256':hashlib.sha256(text.encode()).hexdigest(),'result':r.stdout.strip(),'images':[p.name for p in sorted(out.glob('*.png'))]},indent=2)+'\n')
