#!/usr/bin/env python3
"""Fault-test the production native LAN Play probe with real UDP sockets."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'console/ldn_mitm/source/lanplay_direct.cpp')
parser.add_argument('--report', type=Path)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('discovery_route_test.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
source = args.source.read_text()
probe = extract.function(source, 'bool Probe(')
harness = r'''
#include "lanplay_direct.hpp"
#include "lanplay_codec.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
[[maybe_unused]] static uint64_t monotonic_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static uint32_t lanplay_random(){return 0x3c2b1a09;}
namespace ams::mitm::lanplay {
/* PROBE */
}
static void interrupt_poll(int){}
int main(int argc,char**argv){
 if(argc!=5)return 2;
 ams::mitm::lanplay::Endpoint endpoint{};
 if(inet_pton(AF_INET,argv[1],&endpoint.ipv4)!=1)return 3;
 endpoint.port=htons(std::strtoul(argv[2],nullptr,10));
 if(!std::strcmp(argv[4],"exhaust")){
  rlimit limits{48,48};if(setrlimit(RLIMIT_NOFILE,&limits))return 4;
  while(::socket(AF_INET,SOCK_DGRAM,0)>=0){}
 }
 if(!std::strcmp(argv[4],"interrupt")){
  struct sigaction action{};action.sa_handler=interrupt_poll;sigemptyset(&action.sa_mask);
  if(sigaction(SIGALRM,&action,nullptr))return 5;
  itimerval timer{};timer.it_value.tv_usec=40000;if(setitimer(ITIMER_REAL,&timer,nullptr))return 6;
 }
 const uint64_t start=monotonic_ms();
 const bool result=ams::mitm::lanplay::Probe(endpoint,std::strtoul(argv[3],nullptr,10));
 const auto elapsed=monotonic_ms()-start;
 std::printf("%d %llu\n",result?1:0,static_cast<unsigned long long>(elapsed));
 return 0;
}
'''.replace('/* PROBE */', probe)

cases = [
    ('echo5', True), ('echo4', True), ('silence', False),
    ('delayed', True), ('too_late', False),
    ('wrong_nonce', False), ('wrong_nonce_then_valid', True),
    ('wrong_source', False), ('wrong_source_then_valid', True),
    ('zero', False), ('zero_then_valid', True),
    ('short', False), ('short_then_valid', True),
    ('oversize', False), ('oversize_then_valid', True),
    ('flood', False), ('flood_then_valid', True),
    ('interrupt', True), ('exhaust', False), ('broadcast_error', False),
    ('zero_timeout', False),
]
results = []
with tempfile.TemporaryDirectory(prefix='native-lanplay-probe-') as temp:
    folder = Path(temp)
    cpp, binary = folder / 'probe.cpp', folder / 'probe'
    cpp.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX','c++')) + [
        '-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-I'+str(ROOT/'console/ldn_mitm/source'),str(cpp),'-o',str(binary)
    ], check=True)
    for name, expected in cases:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1',0))
            server.settimeout(3)
            port=server.getsockname()[1]
            errors=[]
            received=[]
            def reply():
                if name in ('exhaust','broadcast_error'):return
                try:
                    try: packet,peer=server.recvfrom(16)
                    except socket.timeout:return
                    received.append(packet.hex())
                    assert packet==bytes.fromhex('02091a2b3c'),packet.hex()
                    if name in ('silence','zero_timeout'):return
                    if name in ('delayed','interrupt'):time.sleep(.10)
                    if name=='too_late':time.sleep(.30)
                    if name.startswith('flood'):
                        end=time.monotonic()+.24
                        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as wrong:
                            while time.monotonic()<end:
                                wrong.sendto(packet,peer)
                                if name=='flood_then_valid' and time.monotonic()>end-.17:
                                    server.sendto(packet,peer)
                                time.sleep(.0005)
                        return
                    if name.startswith('wrong_source'):
                        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as wrong:wrong.sendto(packet,peer)
                    else:
                        payload=(packet[:4] if name=='echo4' else bytes([2,0,0,0,0]) if name.startswith('wrong_nonce')
                                 else b'' if name.startswith('zero') else packet[:2] if name.startswith('short')
                                 else packet+b'X'*2000 if name.startswith('oversize') else packet)
                        server.sendto(payload,peer)
                    if name.endswith('_then_valid'):
                        time.sleep(.02)
                        server.sendto(packet,peer)
                except Exception as error:errors.append(repr(error))
            thread=threading.Thread(target=reply)
            thread.start()
            host='255.255.255.255' if name=='broadcast_error' else '127.0.0.1'
            timeout='0' if name=='zero_timeout' else '180'
            result=subprocess.run([str(binary),host,str(port),timeout,name],capture_output=True,text=True,timeout=5)
            thread.join(timeout=4)
            assert not thread.is_alive() and not errors,(name,errors)
            assert result.returncode==0,(name,result.stderr)
            fields=result.stdout.strip().split()
            assert len(fields)==2,(name,result.stdout,result.stderr)
            actual,elapsed=bool(int(fields[0])),int(fields[1])
            entry={'name':name,'expected':expected,'actual':actual,'server_received':bool(received),'elapsed_ms':elapsed,'passed':actual==expected and elapsed<650}
            results.append(entry)
            print(json.dumps(entry),flush=True)
report={'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'source':str(args.source),
        'transport':'real loopback UDP sockets','sanitizers':['address','undefined'],
        'hardware_tested':False,'cases':results,'passed':all(row['passed'] for row in results)}
if args.report:
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+'\n')
assert report['passed'],[row['name'] for row in results if not row['passed']]
print(f'PASS {len(results)} actual native Probe socket cases, ASan/UBSan, bounded completion')
