#!/usr/bin/env python3
"""Exercise the app's host-only NS lookup and generation-bound IPC write."""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('routes', root / 'app/tests/input_routing_test.py')
routes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routes)
source = (root / 'app/source/main.c').read_text(encoding='utf-8')
publisher_type = re.search(r'typedef struct\s*\{[^}]*\}\s*HostGameVersionPublisher;', source).group()
functions = '\n\n'.join(routes.function(source, 'static bool ' + name + '(') for name in
    ('unified_ipc_missing', 'read_installed_game_version', 'publish_host_game_version',
     'block_incompatible_game_update'))
harness = r'''
#define _POSIX_C_SOURCE 200809L
#include "lobby_model.h"
#include "../../common/game_update_compatibility.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef unsigned Result;
typedef struct {
    struct { char display_version[16]; u64 local_communication_id[8]; } nacp;
    char icon[32];
} NsApplicationControlData;
enum { StateIdle, StateConnecting, StateInRoom };
typedef struct {
    bool status_valid;
    struct { int state, my_index; u64 game_id; } st;
    LobbyGroup groups[LOBBY_PAGE_SIZE];
    LobbyGameVersion room_versions[LOBBY_PAGE_SIZE];
    int count;
    char message[128];
} View;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define MAKERESULT(m,d) ((m) | ((d) << 9))
#define NsApplicationControlSource_StorageOnly 1

static bool g_ns_ready = true, installed = true, target_missing, generation_changed;
static bool alternate_installed, alternate_check_fails;
static atomic_bool g_inventory_running;
static unsigned reads, writes, targets, compatibility_checks, alternate_checks;
static RoomGameVersionTarget active = {.application_id = 0x0100152000022000UL, .generation = 41};
static char local_version[16] = "3.0.5";
static char alternate_version[16] = "3.0.0";
static u64 local_lcid = 1;
static u64 reply_size = sizeof(NsApplicationControlData);
static Result nsIsAnyApplicationEntityInstalled(u64 id, bool *out) {
    if (id != active.application_id) {
        assert(id == room_game_update_alternate(active.application_id));
        alternate_checks++;
        *out = alternate_installed;
        return alternate_check_fails ? 1 : 0;
    }
    *out = installed;
    return 0;
}
static Result nsGetApplicationControlData(int source, u64 id, NsApplicationControlData *out, size_t capacity, u64 *size) {
    assert(source == NsApplicationControlSource_StorageOnly && capacity == sizeof(*out));
    assert(id == active.application_id || id == room_game_update_alternate(active.application_id));
    reads++;
    *size = reply_size;
    memcpy(out->nacp.display_version, id == active.application_id ? local_version : alternate_version,
           sizeof(local_version));
    memset(out->nacp.local_communication_id, 0, sizeof(out->nacp.local_communication_id));
    out->nacp.local_communication_id[0] = local_lcid;
    if (generation_changed) active.generation++;
    return 0;
}
static Result get_target(unsigned command, RoomGameVersionTarget *out) {
    assert(command == ROOM_GAME_VERSION_HOST_IPC);
    targets++;
    if (target_missing) return MAKERESULT(10,221);
    *out = active;
    return 0;
}
static Result send_version(unsigned command, RoomGameVersionUpdate update) {
    assert(command == ROOM_GAME_VERSION_PUBLISH_IPC && update.target.application_id == active.application_id);
    assert(room_game_version_valid(update.game_version, false));
    writes++;
    return update.target.generation == active.generation ? 0 : 1;
}
static const char *ui_text(const char *text) { return text; }
static bool counted_room_game_updates_incompatible(u64 id, const char *host, const char *local) {
    assert(id == active.application_id);
    compatibility_checks++;
    return room_game_updates_incompatible(id, host, local);
}
#define room_game_updates_incompatible counted_room_game_updates_incompatible
#define serviceDispatchOut(service,command,target) get_target(command, &(target))
#define serviceDispatchIn(service,command,update) send_version(command,update)
''' + publisher_type + '\n' + functions + r'''

int main(void) {
    HostGameVersionPublisher publisher = {0};
    View v = {.status_valid = true, .st = {.state = StateInRoom, .my_index = 1, .game_id = 1}};
    assert(!publish_host_game_version(&publisher, &v, 0) && !reads && !writes && !targets);
    v.st.my_index = 0;
    atomic_store(&g_inventory_running, true);
    assert(!publish_host_game_version(&publisher, &v, 0) && !targets);
    atomic_store(&g_inventory_running, false);
    v.st.game_id = 0;
    assert(!publish_host_game_version(&publisher, &v, 0) && !targets);
    v.st.game_id = 1;
    installed = false;
    assert(!publish_host_game_version(&publisher, &v, 0) && !reads && !writes);
    installed = true;
    reply_size = 4;
    assert(!publish_host_game_version(&publisher, &v, 1000) && reads == 1 && !writes);
    reply_size = sizeof(NsApplicationControlData);
    strcpy(local_version, "not-a-version");
    assert(!publish_host_game_version(&publisher, &v, 2000) && reads == 2 && !writes);
    assert(!publish_host_game_version(&publisher, &v, 3000) && reads == 2);

    active.generation++;
    strcpy(local_version, "V3.0.5");
    generation_changed = true;
    assert(!publish_host_game_version(&publisher, &v, 4000) && reads == 3 && writes == 1);
    assert(!publish_host_game_version(&publisher, &v, 4250) && reads == 3); /* Bounded retry. */
    generation_changed = false;
    assert(publish_host_game_version(&publisher, &v, 5000) && reads == 4 && writes == 2);
    assert(!publish_host_game_version(&publisher, &v, 6000) && reads == 4 && writes == 2);

    target_missing = true;
    assert(!publish_host_game_version(&publisher, &v, 7000) && publisher.unsupported);
    const unsigned last_targets = targets;
    assert(!publish_host_game_version(&publisher, &v, 8000) && targets == last_targets);
    LobbyGameVersion version = {0};
    LobbyGroup room = {.game_id = 2, .application_id = active.application_id, .metadata_flags = LOBBY_META_HOST};
    assert(!read_installed_game_version(active.application_id, &room, version) && !version[0]);
    room.game_id = local_lcid;
    assert(read_installed_game_version(active.application_id, &room, version) && !strcmp(version, "V3.0.5"));
    v.count = 1;
    v.groups[0] = room;
    assert(!block_incompatible_game_update(&v, -1) && !compatibility_checks);
    assert(!block_incompatible_game_update(&v, 0) && !compatibility_checks);
    strcpy(local_version, "V4.0.0");
    strcpy(v.room_versions[0], "4.0.1");
    v.groups[0].metadata_flags = LOBBY_META_LOCAL;
    assert(!block_incompatible_game_update(&v, 0) && !compatibility_checks);
    v.groups[0].metadata_flags = LOBBY_META_HOST;
    v.groups[0].game_id = 93;
    assert(!block_incompatible_game_update(&v, 0) && !compatibility_checks);
    v.groups[0].game_id = local_lcid;
    assert(block_incompatible_game_update(&v, 0) && compatibility_checks == 1);
    assert(strstr(v.message, "Yours V4.0.0") && strstr(v.message, "Host 4.0.1"));
    strcpy(v.room_versions[0], "4.0.2");
    assert(!block_incompatible_game_update(&v, 0) && compatibility_checks == 2);
    assert(!alternate_checks);

    active.application_id = UINT64_C(0x0100A3D008C5C000); /* Scarlet room; same LCID as Violet. */
    v.groups[0].application_id = active.application_id;
    strcpy(v.room_versions[0], "3.0.0");
    strcpy(local_version, "3.0.1");
    alternate_installed = true; /* Violet 3.0.0 can be the edition the player opens. */
    const unsigned before = reads;
    assert(!block_incompatible_game_update(&v, 0) && reads == before && compatibility_checks == 2);
    alternate_installed = false;
    alternate_check_fails = true;
    assert(!block_incompatible_game_update(&v, 0) && reads == before && compatibility_checks == 2);
    alternate_check_fails = false;
    assert(block_incompatible_game_update(&v, 0) && reads == before + 1 && compatibility_checks == 3);
    assert(strstr(v.message, "Yours 3.0.1") && strstr(v.message, "Host 3.0.0"));
    assert(alternate_checks == 3);

    alternate_installed = true;
    target_missing = false;
    publisher = (HostGameVersionPublisher){0};
    assert(publish_host_game_version(&publisher, &v, 9000) && alternate_checks == 3);
    puts("PASS fresh NS version, stale-room rejection, old-module fallback, exact incompatibility and dual-edition bypass");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-app-room-version-') as temporary:
    directory = Path(temporary)
    fixture, executable = directory / 'fixture.c', directory / 'fixture'
    fixture.write_text(harness, encoding='utf-8')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
        ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'app/source'),
         str(fixture), str(root / 'app/source/lobby_model.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
