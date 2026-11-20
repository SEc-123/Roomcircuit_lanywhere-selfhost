#!/usr/bin/env python3
"""Render the actual updater status screen with explicit sample states."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / 'source'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
text=(SOURCE/'update_ui.c').read_text()
version=json.loads((SOURCE.parents[1]/'package.json').read_text())['version']
major,minor,patch=map(int,version.split('.'))
fixture_version=f'{major}.{minor}.{patch + 1}'

def function(signature):
    start=text.index(signature);depth=0
    for position in range(text.index('{',start),len(text)):
        if text[position]=='{':depth+=1
        elif text[position]=='}':
            depth-=1
            if not depth:return text[start:position+1]
    raise ValueError(signature)

constants='\n'.join(line for line in text.splitlines() if line.startswith('static const u32'))
types=re.search(r'typedef enum \{.*?\} UpdateScreen;',text,re.S).group()+'\n'+re.search(r'typedef struct \{.*?\} UpdateView;',text,re.S).group()
body='''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "ui_text.h"
#include "update_core.h"
#define LANYWHERE_APP_VERSION @APP_VERSION@
#define SAMPLE_UPDATE_VERSION @FIXTURE_VERSION@
static bool rollback_pending;
static bool language_save_failed;
'''+constants+'\n'+types+'\n'+function('static const char *screen_title(')+'\n'+function('static void draw_release_note(')+'\n'+function('static void draw_update(')+r'''
int main(int argc,char **argv) {
    assert(argc==2 && gfx_init());
    unsigned captures=0;
    for(int chinese=0;chinese<2;chinese++) {
        ui_set_language(chinese,false);
        for(int screen=0;screen<=UpdateRecoveryBlocked;screen++) {
            UpdateView view={.screen=screen,.completed=37,.total=100};
            strcpy(view.manifest.version, screen == UpdateCurrent ? LANYWHERE_APP_VERSION : SAMPLE_UPDATE_VERSION);
            strcpy(view.manifest.notes_en,"See both versions and what's new before installing an update.");
            strcpy(view.manifest.notes_zh,"在线更新前可查看当前版本、目标版本和更新内容。");
            if(screen==UpdateFailed)view.error="Update server is unavailable. Try again.";
            if(screen==UpdateRecoveryBlocked)view.error="Update recovery needs the complete SD package.";
            draw_update(&view);
            char path[4096];snprintf(path,sizeof(path),"%s/update-%s-%d.png",argv[1],chinese?"zh":"en",screen);
            assert(gfx_save_png(path));captures++;
            assert(gfx_text_width(34,ui_text(screen_title(screen)))<1168);
            if(chinese)assert(strcmp(ui_text(screen_title(screen)),screen_title(screen)));
        }
    }
    gfx_exit();printf("Updater: %u bilingual state previews\n",captures);
}
'''
body=body.replace('@APP_VERSION@', json.dumps(version)).replace('@FIXTURE_VERSION@', json.dumps(fixture_version))
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','freetype2','libpng'],text=True))
with tempfile.TemporaryDirectory(prefix='update-preview-') as temporary:
    temp=Path(temporary);(temp/'preview.c').write_text(body)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(HERE),'-I'+str(SOURCE),str(temp/'preview.c'),str(SOURCE/'gfx.c'),str(SOURCE/'ui_text.c'),str(HERE/'host_switch.c'),'-o',str(temp/'preview')]+flags+['-lm'],check=True)
    result=subprocess.run([str(temp/'preview'),str(output)],check=True,capture_output=True,text=True)
    print(result.stdout,end='')
(output/'preview-report.json').write_text(json.dumps({'scope':'actual update_ui.c drawing with sample state, host fonts','physicalSwitch':False,'result':result.stdout.strip(),'sourceSha256':hashlib.sha256(text.encode()).hexdigest(),'images':[x.name for x in sorted(output.glob('*.png'))]},indent=2)+'\n')
