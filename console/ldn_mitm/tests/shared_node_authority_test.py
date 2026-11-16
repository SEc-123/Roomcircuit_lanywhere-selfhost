#!/usr/bin/env python3
"""Exercise the actual verified room cache against forged and expired IPC requests."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('discovery_route_test.py'))
extract = importlib.util.module_from_spec(spec); spec.loader.exec_module(extract)
src=(root/'console/ldn_mitm/source/lanywhere_relay.cpp').read_text()
fn=extract.function(src,'bool VerifiedSharedRoom(const NodeRoomSelection &selection, NodeDynamicEntry *entry) {')
code=r'''
#include "../../../common/node_lobby.h"
#include <cassert>
#include <cstring>
#include <mutex>
#include <cstdio>
using u32=unsigned; using u64=unsigned long long;
#define NODE_CATALOG_COUNT 13
struct VerifiedNodeRoom { NodeRoomSelection selection; NodeDynamicEntry entry; u64 expires_at; };
static std::mutex g_verified_rooms_lock;
static VerifiedNodeRoom g_verified_rooms[48]{};
static u64 time_now=100;
static u64 SelectionNowMs(){return time_now;}
''' + fn + r'''
int main(){
 NodeRoomSelection r{};r.node_id=13;r.game_id=123;r.locator.reserved[0]=8;r.locator.reserved[1]=8;r.locator.reserved[2]=8;r.locator.reserved[3]=8;
 NodeDynamicEntry entry{};entry.id=13;entry.port=11451;strcpy(entry.host,"host.example");
 g_verified_rooms[0].selection=r;g_verified_rooms[0].entry=entry;g_verified_rooms[0].expires_at=200;
 NodeDynamicEntry out{};assert(VerifiedSharedRoom(r,&out)&&out.port==11451);
 auto bad=r;bad.locator.reserved[3]=9;assert(!VerifiedSharedRoom(bad,&out));
 bad=r;bad.node_id=14;assert(!VerifiedSharedRoom(bad,&out));
 bad=r;bad.game_id=124;assert(!VerifiedSharedRoom(bad,&out));
 bad=r;bad.locator.network_id[1]=1;assert(!VerifiedSharedRoom(bad,&out));
 bad=r;bad.node_id=2;assert(!VerifiedSharedRoom(bad,&out));
 assert(!VerifiedSharedRoom(r,nullptr));time_now=200;assert(!VerifiedSharedRoom(r,&out));
 puts("PASS exact directory-authorized room, pinned endpoint, cross-id, game and expiry boundaries");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-room-authority-') as directory:
    d=Path(directory);(d/'test.cpp').write_text(code.replace('#include "../../../common/node_lobby.h"', '#include "'+str(root/'common/node_lobby.h')+'"'))
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True,timeout=10)
