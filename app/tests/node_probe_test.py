#!/usr/bin/env python3
"""Measure actual node probe code over loopback TCP/UDP and exercise ranking."""
from pathlib import Path
import json
import os
import shlex
import socket
import subprocess
import tempfile
import threading
import time
root = Path(__file__).resolve().parents[2]
source = r'''
#include "node_probe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
static atomic_bool cancel;
static void *stop(void *unused){(void)unused;struct timespec delay={0,50000000};nanosleep(&delay,NULL);atomic_store(&cancel,true);return NULL;}
int main(int argc,char **argv){
 assert(argc==5);NodeMeasurement value;NodeProbeGuard guard={0};pthread_t thread;
 const bool cancelling=atoi(argv[4])!=0;if(cancelling)assert(!pthread_create(&thread,NULL,stop,NULL));
 const uint64_t began=node_probe_now_ms();node_probe("127.0.0.1",(uint16_t)atoi(argv[2]),atoi(argv[1]),7,&cancel,&guard,&value);
 if(cancelling)pthread_join(thread,NULL);
 printf("{\"sent\":%u,\"received\":%u,\"median\":%u,\"kind\":%u,\"elapsed\":%llu,\"recommended\":%s}\n",
  value.sent,value.received,value.median_ms,value.kind,(unsigned long long)(node_probe_now_ms()-began),node_measurement_recommended(&value)?"true":"false");
 NodeMeasurement good={.node_id=1,.median_ms=60,.jitter_ms=3,.sent=5,.received=5,.state=NodeMeasureReady};
 NodeMeasurement loss={.node_id=2,.median_ms=20,.jitter_ms=2,.sent=5,.received=3,.state=NodeMeasureReady};
 assert(node_measurement_compare(&good,&loss)<0&&!node_measurement_recommended(&loss));
 loss.received=5;assert(node_measurement_compare(&loss,&good)<0);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='node-probe-') as td:
    td=Path(td);cpp=td/'probe.c';binary=td/'probe';cpp.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-D_POSIX_C_SOURCE=200809L','-DNODE_PROBE_TEST_ENDPOINTS=1','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'app/source'),str(cpp),str(root/'app/source/node_probe.c'),'-o',str(binary)],check=True)
    for kind,mode in [(1,'tcp'),(2,'full'),(2,'prefix'),(2,'noise'),(2,'loss'),(2,'cancel')]:
        sock=socket.socket(socket.AF_INET,socket.SOCK_STREAM if kind==1 else socket.SOCK_DGRAM);sock.bind(('127.0.0.1',0));sock.settimeout(.2)
        if kind==1:sock.listen(1)
        stopped=threading.Event()
        def serve():
            count=0
            if kind==1:
                connection=None
                while not stopped.is_set() and connection is None:
                    try:connection,_=sock.accept()
                    except socket.timeout:continue
                if connection is None:return
                connection.settimeout(.2)
                with connection:
                    while not stopped.is_set():
                        try:
                            request=connection.recv(3)
                            if not request:return
                            assert request==b'\1\0\5';time.sleep(.025);connection.sendall(b'\1');connection.sendall(b'\0\x86')
                        except socket.timeout:continue
                        except OSError:return
            else:
                while not stopped.is_set():
                    try:
                        request,peer=sock.recvfrom(64);count+=1;assert len(request)==5 and request[0]==2
                        if mode=='cancel' or (mode=='loss' and count in (2,4)):continue
                        if mode=='noise':sock.sendto(b'\2bad!',peer)
                        sock.sendto(request[:4] if mode=='prefix' else request,peer)
                    except socket.timeout:continue
                    except OSError:return
        thread=threading.Thread(target=serve);thread.start()
        try:
            output=subprocess.check_output([str(binary),str(kind),str(sock.getsockname()[1]),mode,'1' if mode=='cancel' else '0'],text=True,timeout=8)
            result=json.loads(output)
            if mode=='cancel':assert result['elapsed']<600 and not result['received'],result
            elif mode=='loss':assert result['received']==3 and not result['recommended'],result
            else:assert result['received']==5 and result['recommended'],result
            if mode=='tcp':assert result['median']>=20,result
            print('PASS',mode,result)
        finally:
            stopped.set();sock.close();thread.join(timeout=1)
