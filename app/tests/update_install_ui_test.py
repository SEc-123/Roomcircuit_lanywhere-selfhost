#!/usr/bin/env python3
"""Exercise the actual install worker's download, validation and mandatory-exit transitions."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source=(ROOT/'source/update_ui.c').read_text()
def function(signature):
    start=source.index(signature);depth=0
    for position in range(source.index('{',start),len(source)):
        if source[position]=='{':depth+=1
        elif source[position]=='}':
            depth-=1
            if not depth:return source[start:position+1]
    raise ValueError(signature)

types=re.search(r'typedef enum \{.*?\} UpdateScreen;',source,re.S).group()+'\n'+re.search(r'typedef struct \{.*?\} UpdateView;',source,re.S).group()
harness=r'''
#include "update_core.h"
#include "update_network.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define LANYWHERE_APP_VERSION "0.7.0"
#define UpdateRoot "sdmc:"
#define UpdateDownload "sdmc:/lanywhere/update-download.bin"
#define RELAY_PROFILE_IPV4 "49.233.147.207"
#define RELAY_PROFILE_PORT 11460
/* TYPES */
static UpdateView state;
static int lock;
static bool canonical_path=true;
static atomic_bool cancel_requested,rollback_pending,must_exit;
static unsigned safe_calls,download_calls,install_calls,finish_calls,unlinks,lock_calls;
static bool safe_before=true,safe_after=true,cancel_after_download,cancel_at_commit;
static UpdateResult install_result=UPDATE_INSTALLED,recovery_result=UPDATE_OK;
static UpdateNetworkResult network_result=UpdateNetworkOk;
static UpdateView snapshot(void){return state;}
static bool installation_safe(void){return ++safe_calls==1?safe_before:safe_after;}
static void set_state(UpdateScreen screen,const char *error){state.screen=screen;state.error=error;}
static void finish_worker(void){finish_calls++;}
static void set_progress(unsigned current,unsigned total,void *context){(void)context;state.completed=current;state.total=total;}
static void mutexLock(int *value){assert(value==&lock);lock_calls++;if(cancel_at_commit)atomic_store(&cancel_requested,true);}
static void mutexUnlock(int *value){assert(value==&lock);}
static int fake_mkdir(const char *path,unsigned mode){(void)mode;assert(!strcmp(path,"sdmc:/lanywhere"));return 0;}
static int fake_lstat(const char *path,struct stat *st){assert(!strcmp(path,"sdmc:/lanywhere"));memset(st,0,sizeof(*st));st->st_mode=S_IFDIR|0700;return 0;}
static int fake_unlink(const char *path){assert(!strcmp(path,UpdateDownload));unlinks++;return 0;}
UpdateNetworkResult update_network_download(const char *host,uint16_t port,const uint8_t hash[32],uint32_t size,
 const char *destination,const atomic_bool *cancel,UpdateNetworkProgress progress,void *context){
 assert(!strcmp(host,RELAY_PROFILE_IPV4)&&port==RELAY_PROFILE_PORT&&hash[0]==0x42&&size==100&&!strcmp(destination,UpdateDownload));
 assert(cancel==&cancel_requested);download_calls++;progress(100,100,context);
 if(cancel_after_download)atomic_store(&cancel_requested,true);
 return network_result;
}
UpdateResult update_install(const char *root,const UpdateManifest *manifest,const char *path,UpdateProgress progress,void *context){
 assert(!strcmp(root,UpdateRoot)&&!strcmp(path,UpdateDownload)&&manifest->bundle_sha256[0]==0x42);
 assert(state.screen==UpdateInstalling&&!atomic_load(&cancel_requested));install_calls++;progress(23,23,context);return install_result;
}
UpdateResult update_recover(const char *root){assert(!strcmp(root,UpdateRoot));return recovery_result;}
#define mkdir fake_mkdir
#define lstat fake_lstat
#define unlink fake_unlink
/* FUNCTIONS */
static void reset(const char *version){
 state=(UpdateView){0};snprintf(state.manifest.version,sizeof(state.manifest.version),"%s",version);
 state.manifest.bundle_sha256[0]=0x42;state.manifest.compressed_size=100;
 safe_calls=download_calls=install_calls=finish_calls=unlinks=lock_calls=0;
 safe_before=safe_after=canonical_path=true;cancel_after_download=cancel_at_commit=false;
 atomic_store(&cancel_requested,false);atomic_store(&rollback_pending,false);atomic_store(&must_exit,false);
 install_result=UPDATE_INSTALLED;recovery_result=UPDATE_OK;network_result=UpdateNetworkOk;
}
int main(void){
 reset("0.7.0");network_result=UpdateNetworkCancelled;install_worker(NULL);
 assert(state.screen==UpdateCurrent&&!install_calls&&finish_calls==1);
 reset("0.7.1");network_result=UpdateNetworkCancelled;install_worker(NULL);
 assert(state.screen==UpdateOffered&&!install_calls&&finish_calls==1);
 reset("0.7.1");safe_after=false;install_worker(NULL);
 assert(state.screen==UpdateOffered&&!install_calls&&unlinks==1&&!strcmp(state.error,"Close the game from HOME, then try again."));
 reset("0.7.1");install_worker(NULL);
 assert(state.screen==UpdateRestart&&must_exit&&install_calls==1&&finish_calls==1&&unlinks==1);
 reset("0.7.1");install_result=UPDATE_IO;recovery_result=UPDATE_RECOVERED;install_worker(NULL);
 assert(state.screen==UpdateRestart&&must_exit&&rollback_pending&&install_calls==1);
 reset("0.7.1");install_result=UPDATE_IO;recovery_result=UPDATE_RECOVERY_REQUIRED;install_worker(NULL);
 assert(state.screen==UpdateRecoveryBlocked&&install_calls==1);
 reset("0.7.1");network_result=UpdateNetworkUnavailable;install_worker(NULL);
 assert(state.screen==UpdateFailed&&download_calls==1&&!install_calls);
 reset("0.7.1");install_result=UPDATE_SIGNATURE;install_worker(NULL);
 assert(state.screen==UpdateFailed&&install_calls==1&&!must_exit);
 reset("0.7.1");safe_before=false;canonical_path=false;install_worker(NULL);
 assert(state.screen==UpdateFailed&&!download_calls&&!install_calls);
 reset("0.7.0");cancel_after_download=true;install_worker(NULL);
 assert(state.screen==UpdateCurrent&&!install_calls&&unlinks==1);
 reset("0.7.1");cancel_at_commit=true;install_worker(NULL);
 assert(state.screen==UpdateOffered&&!install_calls&&unlinks==1&&lock_calls==1);
 puts("PASS: 11 actual install-worker transitions, cancellation boundaries, active-game recheck, rollback and restart");
}
'''.replace('/* TYPES */',types).replace('/* FUNCTIONS */','\n'.join(function(signature) for signature in ['static const char *network_error(', 'static const char *core_error(', 'static void install_worker(']))
with tempfile.TemporaryDirectory(prefix='update-install-ui-') as temporary:
    directory=Path(temporary);(directory/'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-I'+str(ROOT/'source'),str(directory/'test.c'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True)
