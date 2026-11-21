"""Run actual title/launch functions with a simulated local NS result."""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'app/source/main.c').read_text(encoding='utf-8')
spec = importlib.util.spec_from_file_location('routes', root / 'app/tests/input_routing_test.py')
routes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routes)
game_type = re.search(r'typedef struct\s*\{[^}]*\}\s*GameInfo;', source).group()
functions = '\n\n'.join(routes.function(source, 'static ' + name) for name in
    ('const GameInfo *local_game_info(', 'CatalogMatch room_catalog_match(', 'bool game_info_has_lcid(', 'const char *room_title(',
     'const char *direct_host_title(', 'bool has_pinned_direct_room(', 'bool room_password_allows_launch(',
     'const char *external_join_hint(', 'bool open_selected_game('))
appointments = (root / 'app/source/appointments_view.c').read_text(encoding='utf-8')
functions += '\n\n' + routes.function(appointments, 'static const char *installed_game_name(')
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobby_model.h"
#include "game_catalog.h"
#include "../../common/node_catalog.h"
#include "ui_text.h"
#include "../../common/game_identity_cache.h"
#include "../../common/game_identity_text.h"
typedef uint64_t u64;
typedef struct { void *px; int w,h; } Image;
#define TR(es,en) ui_tr((es),(en))
#define R_FAILED(x) ((x)!=0)
''' + game_type + r'''
typedef struct { LobbyRoomId pinned; LobbyGroup pinned_group; LobbyNodeSelection pinned_node; bool external_confirm;
    RoomPasswordView password; char message[128]; DirectRoomHost direct_host; struct { u64 game_id; } st; } View;
static bool g_zh, installed;
static GameCatalog g_catalog;
static GameInfo fake;
static u64 last_query;
static int launches;
static bool nodes_ui_valid(uint32_t id) { return id < NODE_CATALOG_COUNT || id == 13; }
static const char *nodes_ui_provider(uint32_t id) { return id == 13 ? "custom-13" : LanywhereNodes[id].provider; }
static const GameInfo *game_info(u64 id) { last_query=id; return installed ? &fake : NULL; }
static void draw_busy(const char *s) { (void)s; }
static int appletRequestLaunchApplication(u64 id, const void *p) { (void)p; assert(id==fake.id); launches++; return 0; }
''' + functions + r'''
static size_t load_catalog(const char *path, unsigned char data[512])
{
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t bytes = fread(data, 1, 512, file);
    assert(!ferror(file) && feof(file));
    fclose(file);
    return bytes;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char fallback[96];
    LobbyGroup room={.code="ABC234",.game_id=0x222,.application_id=0x111,.metadata_flags=LOBBY_META_HOST};
    strcpy(room.title_en,"Host game"); strcpy(room.title_zh,"房主游戏");
    fake.id=0x111; fake.installed=true; fake.name_preferred=true; fake.lcids[0]=0x222; strcpy(fake.name,"Local game");
    installed=true; ui_set_language(false,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Local game") && last_query==0x111);
    View v={0};v.pinned.game_id=room.game_id;v.pinned_group=room;
    v.password.pending = true;
    assert(!open_selected_game(&v) && launches == 0);
    v.password.pending = false;
    v.pinned_group.room_access = RoomAccessHidden;
    assert(!open_selected_game(&v) && launches == 0);
    v.password.room.game_id = room.game_id;
    v.password.locked = true;
    assert(!open_selected_game(&v) && launches == 0);
    v.password.unlocked = true;
    assert(open_selected_game(&v) && launches == 1);
    v.external_confirm=true;
    v.password.unlocked=false;
    v.pinned_node.node_id=7; v.pinned_node.game_id=room.game_id;
    strcpy(v.pinned.provider,LanywhereNodes[7].provider);
    memcpy(v.pinned.code,room.code,sizeof(v.pinned.code));
    memcpy(v.pinned_node.locator.code,room.code,sizeof(v.pinned_node.locator.code));
    assert(open_selected_game(&v) && launches == 2); /* Direct password is checked in game. */
    v.pinned_node.locator.code[0]='X';
    assert(!open_selected_game(&v) && launches == 2);
    v.external_confirm=false; v.password.unlocked=true;
    fake.lcids[0]=0x333; launches=0;
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Host game"));
    assert(!open_selected_game(&v) && launches==0); /* claimed ID does not match local LCID */
    room.game_id=UINT64_MAX;fake.lcids[0]=UINT64_MAX;v.pinned.game_id=UINT64_MAX;v.pinned_group=room;
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Host game"));
    assert(!open_selected_game(&v) && launches==0); /* sentinel is not proof, even if NACP contains it */
    room.game_id=0x222;fake.lcids[0]=0x333;v.pinned.game_id=room.game_id;v.pinned_group=room;
    installed=false; last_query=0;
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Host game") && last_query==0x111);
    assert(!open_selected_game(&v) && launches==0); /* remote title must not launch uninstalled game */
    g_zh=true;ui_set_language(true,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"房主游戏"));
    /* An older upstream may host Violet while only Scarlet is installed here;
       the shared LCID can identify a local candidate, never the remote title. */
    room.metadata_flags=LOBBY_META_LOCAL;strcpy(room.title_en,"Pokémon Scarlet");strcpy(room.title_zh,"宝可梦 朱");
    fake.lcids[0]=0x222;strcpy(fake.name,"Pokémon Scarlet");installed=true;
    g_zh=false;ui_set_language(false,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Pokémon Scarlet"));
    installed=false;
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Pokémon Scarlet"));
    room.metadata_flags=LOBBY_META_HOST|LOBBY_META_LOCAL;
    g_zh=true;ui_set_language(true,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"宝可梦 朱"));
    room.metadata_flags=0;room.application_id=0;room.title_en[0]=room.title_zh[0]=0;last_query=0;
    assert(strstr(room_title(&room,fallback,sizeof(fallback)),"0000000000000222") && last_query==0);
    room.game_id=UINT64_MAX;
    v.pinned=lobby_room_id_on(&room,"lanplay-nl"); v.pinned_group=room;
    assert(v.pinned.game_id==UINT64_MAX && !strcmp(v.pinned.code,room.code));
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"未提供游戏信息"));
    g_zh=false;ui_set_language(false,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),"Game info not provided"));
    assert(!local_game_info(&room) && last_query==0 && !open_selected_game(&v) && launches==0);
    assert(!strcmp(external_join_hint(v.pinned.provider,false),"Open the game from HOME, then search in Local Wireless."));

    unsigned char data[512];
    const u64 mario = UINT64_C(0x0100152000022000);
    size_t bytes = load_catalog(argv[1], data);
    assert(game_catalog_open(&g_catalog, data, bytes));
    CatalogMatch match = game_catalog_identify(&g_catalog, mario, 0);
    assert(!strcmp(game_catalog_name(&match, true), "Legacy English") && !match.original);
    bytes = load_catalog(argv[2], data);
    assert(!game_catalog_open(&g_catalog, data, bytes - 1));
    assert(game_catalog_open(&g_catalog, data, bytes));
    const char *mario_en = "Mario Kart 8 Deluxe", *mario_zh = "马力欧卡丁车 8 豪华版";
    room = (LobbyGroup){.game_id=mario,.application_id=mario,.metadata_flags=LOBBY_META_HOST};
    strcpy(room.title_en, mario_en);
    strcpy(room.title_zh, mario_en); /* Host copied an English fallback into both slots. */
    installed=false;
    g_zh=true; ui_set_language(true,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),mario_zh));
    v.direct_host.application_id=mario; v.st.game_id=mario;
    strcpy(v.direct_host.title_en,mario_en); strcpy(v.direct_host.title_zh,mario_en);
    assert(!strcmp(direct_host_title(&v),mario_zh));
    GameCacheRecord saved={.application_id=mario};
    strcpy(saved.names.english,mario_en); strcpy(saved.names.chinese,mario_en);
    assert(!strcmp(installed_game_name(&g_catalog,&saved),mario_zh));
    fake.id=mario; fake.lcids[0]=mario; installed=true;
    fake.name_preferred=false; strcpy(fake.name,mario_en);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),mario_zh));
    fake.name_preferred=true; /* An untranslated local slot can differ in punctuation from the catalog. */
    strcpy(fake.name,"Mario Kart™ 8 Deluxe");
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),mario_zh));
    strcpy(fake.name,"本机游戏名");
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),mario_zh));
    installed=false;
    room.title_en[0]=0; strcpy(room.title_zh,mario_zh);
    g_zh=false; ui_set_language(false,false);
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),mario_en));
    room.application_id=0x112; /* Do not re-identify a distinct host app from an equal-ID clue. */
    assert(!strcmp(room_title(&room,fallback,sizeof(fallback)),mario_zh));

    match = game_catalog_identify(&g_catalog, 0, UINT64_C(0x0101230000004000));
    assert(match.kind == CatalogEqualId && !match.en && !match.zh);
    assert(!strcmp(game_catalog_name(&match, true), "日本語ゲーム"));
    assert(!strcmp(game_display_name(&match, false, NULL, false, "English from host", ""), "English from host"));
    CatalogGameChoice choice; size_t next;
    assert(game_catalog_choices(&g_catalog, "日本語", 0, &choice, 1, &next) == 1 && choice.game_id == match.app_id);
    assert(next == SIZE_MAX);
    puts("Actual UI identity: preferred-language labels, v1/v2 catalog fallback and verified launch passed");
}
'''
with tempfile.TemporaryDirectory(prefix='switch-identity-ui-') as temp:
    temp = Path(temp); cfile=temp/'test.c'; exe=temp/'test';cfile.write_text(harness,encoding='utf-8')
    legacy = temp / 'legacy.bin'
    current = temp / 'current.bin'
    mario = 0x0100152000022000
    old_text = b'\0Legacy English\0'
    legacy.write_bytes(struct.pack('<8sIIIQII', b'LYCAT1\0\0', 1, 0, len(old_text), mario, 1, 0) + old_text)
    labels = b'\0Mario Kart 8 Deluxe\0' + '马力欧卡丁车 8 豪华版\0日本語ゲーム\0'.encode()
    localized = len(b'\0Mario Kart 8 Deluxe\0')
    original = localized + len('马力欧卡丁车 8 豪华版\0'.encode())
    current.write_bytes(struct.pack('<8sIIIQIIIQIII', b'LYCAT2\0\0', 2, 0, len(labels), mario, 1, localized, 0,
                                    0x0101230000004000, 0, 0, original) + labels)
    cc=shlex.split(os.environ.get('CC','cc'))
    subprocess.run(cc + ['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'app/source'),str(cfile),
                         str(root/'app/source/lobby_model.c'),str(root/'app/source/ui_text.c'),str(root/'app/source/game_catalog.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe), str(legacy), str(current)],check=True)
