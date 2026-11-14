#!/usr/bin/env python3
"""Actual updater boot acknowledgement and game-running guards with service boundaries mocked."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'source/update_ui.c').read_text()

def function(signature):
    start = source.index(signature)
    depth = 0
    for position in range(source.index('{', start), len(source)):
        if source[position] == '{': depth += 1
        elif source[position] == '}':
            depth -= 1
            if not depth: return source[start:position + 1]
    raise ValueError(signature)

harness = r'''
#include "update_core.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef unsigned Result;
typedef unsigned long long u64;
typedef int Service;
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define MAKERESULT(m,d) ((m)|((d)<<9))
#define CUR_PROCESS_HANDLE 1
#define UpdateRoot "sdmc:"
#define LANYWHERE_APP_VERSION "0.7.0"
#define UpdateBuildVersionIpc 34
static unsigned recover_result,pending_result,ack_result,pm_init_result,pm_result,svc_result,inspect_result;
static u64 application_pid,current_pid=100;
static const char *pending_version="0.7.0",*module_version="0.7.0";
static bool malformed,initialized,canonical_path,socket_ready,rollback_pending,must_exit;
static int lock,ack_calls,start_calls,recover_calls,inspect_calls;
static struct {int screen;}state;
enum {UpdateChecking,UpdateRestart,UpdateRecoveryBlocked};
static void mutexInit(int *unused){(void)unused;}
static void set_state(int screen,const char *error){(void)error;state.screen=screen;}
static Result socketInitializeDefault(void){return 0;}
static void start_worker(bool install){assert(!install);start_calls++;}
UpdateResult update_recover(const char *root){assert(!strcmp(root,UpdateRoot));recover_calls++;return recover_result;}
UpdateResult update_transaction_pending(const char *root){assert(!strcmp(root,UpdateRoot));inspect_calls++;return inspect_result;}
UpdateResult update_pending_version(const char *root,char out[32]){assert(!strcmp(root,UpdateRoot));snprintf(out,32,"%s",pending_version);return pending_result;}
UpdateResult update_acknowledge(const char *root){assert(!strcmp(root,UpdateRoot));ack_calls++;return ack_result;}
static Result smGetService(Service *out,const char *name){*out=1;assert(!strcmp(name,"lanywhr"));return svc_result;}
static void serviceClose(Service *unused){(void)unused;}
static Result dispatch(char *buffer,size_t size,int command){assert(command==34);if(malformed)memset(buffer,'x',size);else snprintf(buffer,size,"%s",module_version);return 0;}
#define serviceDispatch(s,c,...) dispatch(runtime,sizeof(runtime),c)
static Result pmdmntInitialize(void){return pm_init_result;}
static Result pmdmntGetApplicationProcessId(u64 *out){*out=application_pid;return pm_result;}
static void pmdmntExit(void){}
static Result svcGetProcessId(u64 *out,int handle){assert(handle==CUR_PROCESS_HANDLE);*out=current_pid;return 0;}
/* FUNCTIONS */
static void reset(void){rollback_pending=must_exit=false;initialized=canonical_path=socket_ready=false;ack_calls=start_calls=recover_calls=inspect_calls=0;state.screen=-1;inspect_result=UPDATE_OK;pm_init_result=pm_result=0;application_pid=0;recover_result=UPDATE_OK;pending_result=UPDATE_INSTALLED;ack_result=UPDATE_OK;pending_version=module_version="0.7.0";svc_result=0;malformed=false;}
int main(void){
 reset();assert(!update_ui_initialize("sdmc:/switch/lanywhere.nro")&&canonical_path&&start_calls==1&&!ack_calls);
 reset();recover_result=UPDATE_INSTALLED;assert(!update_ui_initialize("sdmc:/switch/lanywhere.nro")&&ack_calls==1&&start_calls==1);
 reset();recover_result=pending_result=UPDATE_RECOVERED;assert(!update_ui_initialize("sdmc:/switch/lanywhere.nro")&&ack_calls==1);
 reset();recover_result=UPDATE_INSTALLED;pending_version="0.8.0";assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!ack_calls&&state.screen==UpdateRestart);
 reset();recover_result=UPDATE_INSTALLED;module_version="0.6.2";assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!ack_calls);
 reset();recover_result=UPDATE_INSTALLED;pending_result=UPDATE_IO;assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!ack_calls);
 reset();recover_result=UPDATE_INSTALLED;svc_result=1;assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!ack_calls);
 reset();recover_result=UPDATE_INSTALLED;malformed=true;assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!ack_calls);
 reset();recover_result=pending_result=UPDATE_RECOVERED;module_version="0.6.2";assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&rollback_pending);
 reset();recover_result=pending_result=UPDATE_RECOVERED;pending_version="0.6.2";assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!rollback_pending);
 reset();recover_result=pending_result=UPDATE_RECOVERED;module_version="0.6.2";assert(update_ui_initialize("sdmc:/switch/lanywhere-recovery.nro")&&!rollback_pending);
 reset();recover_result=UPDATE_RECOVERY_REQUIRED;assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&!start_calls&&state.screen==UpdateRecoveryBlocked);
 reset();assert(!update_ui_initialize("sdmc:/switch/old.nro")&&!canonical_path);assert(!game_closed() || !canonical_path);
 reset();application_pid=200;inspect_result=UPDATE_RECOVERY_REQUIRED;recover_result=UPDATE_RECOVERED;
 assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&recover_calls==0&&inspect_calls==1&&!ack_calls&&!start_calls);
 reset();pm_init_result=1;inspect_result=UPDATE_RECOVERY_REQUIRED;
 assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&recover_calls==0&&inspect_calls==1&&!ack_calls);
 reset();pm_result=MAKERESULT(15,7);inspect_result=UPDATE_RECOVERY_REQUIRED;
 assert(update_ui_initialize("sdmc:/switch/lanywhere.nro")&&recover_calls==0&&inspect_calls==1);
 reset();application_pid=200;inspect_result=UPDATE_OK;
 assert(!update_ui_initialize("sdmc:/switch/lanywhere.nro")&&recover_calls==0&&inspect_calls==1&&start_calls==1);
 reset();pm_init_result=1;inspect_result=UPDATE_OK;
 assert(!update_ui_initialize("sdmc:/switch/lanywhere.nro")&&recover_calls==0&&inspect_calls==1&&start_calls==1);
 reset();application_pid=current_pid;recover_result=UPDATE_INSTALLED;
 assert(!update_ui_initialize("sdmc:/switch/lanywhere.nro")&&recover_calls==1&&!inspect_calls&&ack_calls==1);
 pm_init_result=0;pm_result=MAKERESULT(15,1);assert(game_closed());
 pm_result=0;application_pid=current_pid;assert(game_closed());
 application_pid=200;assert(!game_closed());
 pm_result=MAKERESULT(15,7);assert(!game_closed());
 pm_init_result=1;assert(!game_closed());
 puts("PASS: updater recovery ownership, app/runtime match, active/unknown application and staging guards");
}
'''.replace('/* FUNCTIONS */','\n'.join(function(signature) for signature in ['static bool game_closed(', 'static bool running_version_matches(', 'bool update_ui_initialize(']))
with tempfile.TemporaryDirectory(prefix='updater-startup-') as temporary:
    directory=Path(temporary);(directory/'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'source'),str(directory/'test.c'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True)
