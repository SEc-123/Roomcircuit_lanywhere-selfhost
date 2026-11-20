/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
// LANywhere: panel para jugar en línea con el modo inalámbrico local.
//
// Muestra si el servidor responde, los grupos abiertos de todos los juegos (los crea cualquiera con "crear grupo"
// en el modo inalámbrico local) y permite elegir si los grupos propios se ven, o jugar en una sala privada por
// código. Game networking uses the sysmodule; the updater connects to the release service directly.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <switch.h>
#include <switch/sf/tipc.h>
#include <turbojpeg.h>
#include "gfx.h"
#include "lobby_model.h"
#include "nacp_titles.h"
#include "guide.h"
#include "ui_text.h"
#include "ui_locale.h"
#include "relay_config.h"
#include "title_inventory.h"
#include "game_catalog.h"
#include "game_catalog_bin.h"
#include "appointments_model.h"
#include "appointments_view.h"
#include "room_password.h"
#include "update_ui.h"
#include "nodes_ui.h"
#include "roomcircuit_storage.h"
#include "../../common/roomcircuit_paths.h"

#define TOUCH_UPDATE (1ULL << 59)
#define TOUCH_NODE_FILTER (1ULL << 55)

#define MIN_REFRESH_MS   1250
#define STATUS_POLL_MS    250
#define ROOM_NOTICE_MS   6000
#define MAX_PAGE_HISTORY 1024

static bool g_es;
static bool g_zh;
static bool g_guide_open;
static int g_guide_page;
#define TR(es, en) ui_tr((es), (en))

static void detect_language(void)
{
    g_es = false;
    ui_locale_initialize();
    g_zh = ui_chinese();
}

// Los errores del sysmodule llegan en español: traducción de los conocidos.
static const char *tr_error(const char *es)
{
    if (strcmp(es, "Set a valid server IPv4 and port in the LANywhere app") == 0) {
        if (RELAY_PROFILE_FIXED)
            return TR("Reinstala los archivos de RoomCircuit del mismo paquete.", "Reinstall the RoomCircuit files from the same package.");
        return TR("Configura una IPv4 y un puerto válidos en RoomCircuit.", "Set a valid server IPv4 and port in RoomCircuit.");
    }
    static const char *const map[][2] = {
        { "la sala no existe", "that room does not exist" },
        { "la sala está llena", "the room is full" },
        { "la sala es de otro juego", "that room is for another game" },
        { "no se pudo conectar con el servidor", "could not connect to the server" },
        { "el servidor no responde", "the server is not responding" },
        { "el servidor no contestó", "the server did not answer" },
        { "el servidor no recibe datos (UDP)", "the server is not receiving data (UDP)" },
        { "se perdió la conexión con el servidor", "lost the connection to the server" },
        { "reconectando con el servidor", "reconnecting to the server" },
        { "el puesto en la sala ya no existe", "your place in the room no longer exists" },
        { "el servidor llegó a su límite de este mes; vuelve el día 1", "the server reached its limit for this month; back on the 1st" },
        { "demasiadas salas; prueba en un momento", "too many rooms; try again in a moment" },
        { "demasiados intentos; prueba en un momento", "too many attempts; try again in a moment" },
        { "no se pudo crear el socket", "could not create a network socket" },
        { "respuesta inesperada", "unexpected server response" },
        { "no hay direcciones libres; prueba en un momento", "no addresses available; try again later" },
        { "el anfitrión no confirmó la conexión al juego", "the host did not confirm the game connection" },
        { "la conexión anterior todavía está activa", "the previous connection is still active" },
        { "el servidor se está cerrando", "the server is shutting down" },
        { "CREATE no válido", "invalid create request" },
        { "JOIN no válido", "invalid join request" },
        { "actualiza LANywhere para entrar en esta sala", "Update RoomCircuit to join this room." },
        { "Reopen LANywhere to read game information.", "Reopen RoomCircuit to read game information." },
        { "JOIN sin juego", "join request missing game" },
        { "la versión local del juego no coincide", "local game versions do not match" },
        { "la reserva ya no está disponible", "the reservation is no longer available" },
        { "ya estás en una sala", "you are already in a room" },
        { "el grupo no está abierto", "the room is not open" },
        { "RESUME no válido", "invalid reconnect request" },
        { "LEAVE no válido", "invalid leave request" },
        { "solo el anfitrión puede publicar", "only the host can publish" },
        { "grupo no válido", "invalid room information" },
        { "FIND no válido", "invalid search request" },
        { "reserva no válida", "invalid reservation" },
        { "demasiadas peticiones de búsqueda", "too many match requests" },
        { "demasiadas reservas", "too many reservations" },
        { "CAPS no válido", "invalid capability request" },
        { "FIND_PAGE no válido", "invalid room page request" },
        { "demasiadas búsquedas", "too many searches" },
        { "suscripción no válida", "invalid alert subscription" },
        { "demasiadas suscripciones", "too many alert subscriptions; try again shortly" },
        { "la conexión de suscripción es de solo lectura", "the alert connection is read-only" },
        { "filtro de búsqueda no válido", "invalid search filter" },
        { "PING no válido", "invalid ping request" },
        { "el destinatario necesita actualizar el transporte UDP", "a player needs an updated UDP transport" },
        { "demasiadas actualizaciones del nombre del juego", "too many game title updates" },
        { "metadatos del anfitrión no válidos", "invalid host game information" },
        { "página de metadatos no válida", "invalid game information page" },
    };
    if (g_es)
        return es;
    /* This known long server error can arrive truncated in the fixed-size status buffer. */
    const char *paused_prefix = "el servidor llegó a su límite de este mes";
    if (strncmp(es, paused_prefix, strlen(paused_prefix)) == 0)
        return ui_text("the server reached its monthly limit");
    /* Unknown message types are numeric; keep all other unfamiliar text unchanged. */
    if (strncmp(es, "tipo ", 5) == 0) {
        const char *p = es + 5;
        const char *first = p;
        while (*p >= '0' && *p <= '9') ++p;
        if (p != first && strcmp(p, " desconocido") == 0)
            return ui_text("unknown server message type");
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(es, map[i][0]) == 0)
            return ui_text(map[i][1]);
    return ui_text(es);
}

// --- servicio "lanywhr" ---

typedef struct {
    u8 state, my_index, member_count, padding;
    u32 my_ip;
    char code[8];
    u64 game_id;
    u32 packets_sent, packets_received;
    char error[64];
} LanywhereStatus;
_Static_assert(sizeof(LanywhereStatus) == 0x60, "igual que lanywhere::Status");

enum { StateIdle = 0, StateConnecting = 1, StateInRoom = 2, StateError = 3 };

static Service g_lanywhere;
static Service g_lanywhere_list;  // sesión propia del hilo de la lista

static Result lanywhere_create(u64 game_id)
{
    return serviceDispatchIn(&g_lanywhere, 0, game_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { "", 0 } });
}

static Result lanywhere_join(u64 game_id, const char *code)
{
    return serviceDispatchIn(&g_lanywhere, 1, game_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In, SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { code, strlen(code) }, { "", 0 } });
}

static Result lanywhere_leave(void)
{
    return serviceDispatch(&g_lanywhere, 2);
}

static Result lanywhere_status(LanywhereStatus *out)
{
    return serviceDispatchOut(&g_lanywhere, 3, *out);
}

static Result lanywhere_select(const char *code)
{
    return serviceDispatch(&g_lanywhere, 6,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { code, strlen(code) } });
}

static Result lanywhere_select_unified(LobbyRoomId id)
{
    UnifiedRoomRequest request = {.game_id = id.game_id};
    memcpy(request.code, id.code, sizeof(request.code));
    memcpy(request.provider, id.provider, sizeof(request.provider));
    return serviceDispatchIn(&g_lanywhere, UNIFIED_LOBBY_IPC_SELECT, request);
}

static Result lanywhere_select_node(LobbyNodeSelection request)
{
    return serviceDispatchIn(&g_lanywhere,38,request);
}

static Result lanywhere_start_quick(void)
{
    return serviceDispatch(&g_lanywhere, 7);
}

static Result lanywhere_cancel_match(void)
{
    return serviceDispatch(&g_lanywhere, 8);
}

static Result lanywhere_match_status(LobbyMatch *out)
{
    return serviceDispatchOut(&g_lanywhere, 9, *out);
}

static bool lanywhere_host_region(u32 *region)
{
    return (relay_config_ipv4_valid(RELAY_PROFILE_EU_IPV4) ||
            relay_config_ipv4_valid(RELAY_PROFILE_COMMUNITY_IPV4)) &&
        R_SUCCEEDED(serviceDispatchOut(&g_lanywhere, RELAY_REGION_IPC_GET, *region)) &&
        *region <= RelayRegionCommunity;
}

static Result lanywhere_set_host_region(u32 region)
{
    return serviceDispatchIn(&g_lanywhere, RELAY_REGION_IPC_SET, region);
}

static bool lanywhere_protocol_supported(void)
{
    u32 version = 0;
    return R_SUCCEEDED(serviceDispatchOut(&g_lanywhere, LANYWHERE_IPC_CAPABILITIES_COMMAND, version)) && version == LANYWHERE_IPC_MAJOR;
}

static LobbyPage validate_group_page(Result rc, LobbyPage page, u32 offset, LobbyGroup *out, size_t count)
{
    if (R_FAILED(rc))
        return (LobbyPage){ .count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusNetwork };
    if (page.count == -1 && (page.reserved == 0 || lanywhere_list_status_valid(page.reserved)))
        return (LobbyPage){ .count = -1, .next = LOBBY_PAGE_END,
            .reserved = page.reserved ? page.reserved : ListStatusNetwork };
    if (page.count < 0 || (size_t)page.count > count || page.reserved != 0 ||
        (page.next != LOBBY_PAGE_END && (!page.count || page.next <= offset || page.next - offset != (u32)page.count)))
        return (LobbyPage){ .count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusProtocol };
    for (s32 i = 0; i < page.count; ++i) lobby_sanitize_group(&out[i]);
    return page;
}

static LobbyPage lanywhere_list_groups(u32 offset, LobbyGroup *out, size_t count)
{
    LobbyPage page = { .count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusNetwork };
    Result rc = serviceDispatchInOut(&g_lanywhere_list, LANYWHERE_IPC_LIST_PAGE_COMMAND, offset, page,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { out, count * sizeof(*out) } });
    return validate_group_page(rc, page, offset, out, count);
}

static bool unified_ipc_missing(Result result)
{
    return result == MAKERESULT(10, 221); // Atmosphère CMIF UnknownCommandId.
}

static u32 lanywhere_host_regions(u32 lobby_capabilities)
{
    u32 regions = 0;
    const Result rc = serviceDispatchOut(&g_lanywhere, RELAY_REGIONS_IPC_GET, regions);
    if (R_SUCCEEDED(rc)) return regions;
    if (unified_ipc_missing(rc) && (lobby_capabilities & REGIONAL_LOBBY_CAPABILITY) &&
        relay_config_ipv4_valid(RELAY_PROFILE_EU_IPV4))
        return 1u << RelayRegionEurope;
    return 0;
}

static bool unified_list_requires_fallback(Result rc, UnifiedPage page)
{
    return unified_ipc_missing(rc) || (R_SUCCEEDED(rc) && page.count == -1 &&
        page.next == UNIFIED_LOBBY_END && page.status == ListStatusUpdate);
}

enum LobbyDirectory { LobbyDirectoryUnified, LobbyDirectoryRegional, LobbyDirectoryCommunity };

static LobbyPage lanywhere_list_unified(UnifiedRoomsRequest request, LobbyGroup *groups, LobbySource *sources,
                                        UnifiedPage *info, enum LobbyDiscovery *discovery, enum LobbyDirectory *directory)
{
    LobbyUnifiedGroup rows[LOBBY_PAGE_SIZE] = {0};
    UnifiedPage page = {.count = -1, .next = UNIFIED_LOBBY_END, .status = ListStatusNetwork};
    Result rc;
    while (*directory != LobbyDirectoryUnified) {
        const u32 command = *directory == LobbyDirectoryCommunity ? COMMUNITY_LOBBY_IPC_LIST : REGIONAL_LOBBY_IPC_LIST;
        rc = serviceDispatchInOut(&g_lanywhere_list, command, request, page,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { rows, sizeof(rows) } });
        if (!unified_list_requires_fallback(rc, page)) break;
        *directory = *directory == LobbyDirectoryCommunity ? LobbyDirectoryRegional : LobbyDirectoryUnified;
        if (request.offset || request.snapshot)
            return (LobbyPage){.count = -1, .next = LOBBY_PAGE_END, .reserved = UnifiedListStale};
        memset(rows, 0, sizeof(rows));
        page = (UnifiedPage){.count = -1, .next = UNIFIED_LOBBY_END, .status = ListStatusNetwork};
    }
    if (*directory == LobbyDirectoryUnified) {
        rc = serviceDispatchInOut(&g_lanywhere_list, UNIFIED_LOBBY_IPC_LIST, request, page,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { rows, sizeof(rows) } });
        if (unified_list_requires_fallback(rc, page)) {
            *discovery = LobbyDiscoveryLegacy;
            if (request.offset || request.snapshot)
                return (LobbyPage){.count = -1, .next = LOBBY_PAGE_END, .reserved = UnifiedListStale};
            return lanywhere_list_groups(request.offset, groups, LOBBY_PAGE_SIZE);
        }
    }
    if (R_FAILED(rc))
        return (LobbyPage){.count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusNetwork};
    *discovery = LobbyDiscoveryUnified;
    if (!lobby_unified_page_valid(page, request, rows, LOBBY_PAGE_SIZE, *directory != LobbyDirectoryUnified))
        return (LobbyPage){.count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusProtocol};
    *info = page;
    for (s32 i = 0; i < page.count; ++i) {
        if (*directory != LobbyDirectoryCommunity && strcmp(rows[i].provider, REGION_PROVIDER_COMMUNITY) == 0)
            return (LobbyPage){.count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusProtocol};
        groups[i] = rows[i].group;
        lobby_sanitize_group(&groups[i]);
        memcpy(sources[i], rows[i].provider, sizeof(sources[i]));
    }
    return (LobbyPage){.count = page.count, .next = page.next, .reserved = page.status,
        .capabilities = UNIFIED_LOBBY_CAPABILITY | LOBBY_CAP_RESERVED_MATCH | ROOM_PASSCODE_CAPABILITY |
            ROOM_ACCESS_CAPABILITY | (*directory == LobbyDirectoryRegional && page.count >= 0 ? REGIONAL_LOBBY_CAPABILITY : 0)};
}

static LobbyPage lanywhere_list_nodes(NodeRoomsRequest request, LobbyGroup *groups, LobbySource *sources,
                                      LobbyNodeRoom *rows, UnifiedPage *page, bool *supported)
{
    *page = (UnifiedPage){.count=-1,.next=NODE_LOBBY_END,.status=ListStatusNetwork};
    memset(rows,0,sizeof(*rows)*NODE_LOBBY_PAGE_SIZE);
    Result result=serviceDispatchInOut(&g_lanywhere_list,DIRECT_ROOM_LIST_IPC,request,*page,
        .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},
        .buffers={{rows,sizeof(*rows)*NODE_LOBBY_PAGE_SIZE}});
    const bool annotated = !unified_ipc_missing(result);
    if (!annotated)
        result=serviceDispatchInOut(&g_lanywhere_list,37,request,*page,
            .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},
            .buffers={{rows,sizeof(*rows)*NODE_LOBBY_PAGE_SIZE}});
    if(unified_ipc_missing(result) || (R_SUCCEEDED(result)&&page->count==-1&&page->status==ListStatusUpdate)){
        *supported=false;
        return (LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=ListStatusUpdate};
    }
    if(R_FAILED(result))return (LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=ListStatusNetwork};
    if(page->count==-1 && (lanywhere_list_status_valid(page->status)||page->status==UnifiedListStale))
        return (LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=page->status};
    if(page->count<0 || page->count>(int32_t)NODE_LOBBY_PAGE_SIZE || page->status || !page->snapshot ||
       (request.snapshot&&request.snapshot!=page->snapshot) || page->total>NODE_SHARED_CAPACITY || page->available>page->total || (page->next!=NODE_LOBBY_END &&
       (!page->count || page->next!=request.offset+(uint32_t)page->count)))
        return (LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=ListStatusProtocol};
    for(int i=0;i<page->count;i++){
        if(rows[i].node_id>=NODE_SHARED_CAPACITY || !nodes_ui_valid(rows[i].node_id) || !(request.node_mask&(1u<<rows[i].node_id)) ||
           (rows[i].flags!=NodeRoomDirect && rows[i].flags!=NodeRoomGateway) ||
           (request.game_id && rows[i].group.game_id!=request.game_id) ||
           !rows[i].group.game_id || rows[i].group.code[6] || rows[i].group.code[7] ||
           !rows[i].group.max_players || rows[i].group.max_players>8 || rows[i].group.players>rows[i].group.max_players ||
           rows[i].group.room_access>RoomAccessHidden ||
           (rows[i].node_id && rows[i].group.room_access!=RoomAccessNone &&
            (!annotated || rows[i].node_id<2 || rows[i].flags!=NodeRoomDirect)) ||
           rows[i].locator.code[6] || rows[i].locator.code[7] || memcmp(rows[i].locator.code,rows[i].group.code,6) ||
           rows[i].locator.reserved0 ||
           (rows[i].node_id < NODE_CATALOG_COUNT ? memcmp(rows[i].locator.reserved,(uint8_t[12]){0},12) :
             (rows[i].flags != NodeRoomDirect || !memcmp(rows[i].locator.reserved,(uint8_t[4]){0},4) ||
              memcmp(rows[i].locator.reserved+4,(uint8_t[8]){0},8))))
            return (LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=ListStatusProtocol};
        groups[i]=rows[i].group;lobby_sanitize_group(&groups[i]);
        snprintf(sources[i],sizeof(sources[i]),"%s",rows[i].node_id==1 && rows[i].flags==NodeRoomDirect?
            REGION_PROVIDER_COMMUNITY:nodes_ui_provider(rows[i].node_id));
        if(!lobby_room_id_on(&groups[i],sources[i]).game_id)
            return (LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=ListStatusProtocol};
    }
    return (LobbyPage){.count=page->count,.next=page->next,.reserved=0,
        .capabilities=UNIFIED_LOBBY_CAPABILITY|NODE_LOBBY_CAPABILITY|LOBBY_CAP_RESERVED_MATCH|ROOM_PASSCODE_CAPABILITY|ROOM_ACCESS_CAPABILITY};
}

static LobbyPage lanywhere_list_appointment_rooms(u64 game_id, u32 offset, LobbyGroup *out, size_t count)
{
    AppointmentRoomsRequest request = {.game_id = game_id, .offset = offset};
    LobbyPage page = { .count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusNetwork };
    Result rc = serviceDispatchInOut(&g_lanywhere_list, APPOINTMENT_IPC_ROOMS, request, page,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { out, count * sizeof(*out) } });
    page = validate_group_page(rc, page, offset, out, count);
    for (s32 i = 0; i < page.count; i++) {
        if (out[i].game_id != game_id)
            return (LobbyPage){ .count = -1, .next = LOBBY_PAGE_END, .reserved = ListStatusProtocol };
    }
    return page;
}

// --- juegos instalados: nombre e icono ---

typedef struct {
    u64 id;
    u64 lcids[8];
    char name[128];
    Image icon;
    bool installed;
    bool install_checked;
    unsigned attempts;
    u64 retry_after_ms;
} GameInfo;
static GameInfo g_games[24];
static int g_game_count;
static unsigned g_game_replace;
static bool g_ns_ready;
static atomic_bool g_inventory_running;
static atomic_bool g_inventory_finished;
static atomic_bool g_inventory_success;
static GameCatalog g_catalog;

static u64 now_ms(void);

static void inventory_thread(void *arg)
{
    (void)arg;
    atomic_store(&g_inventory_success, title_inventory_refresh());
    atomic_store(&g_inventory_running, false);
    atomic_store(&g_inventory_finished, true);
}

static void decode_icon(Image *img, const unsigned char *jpeg, size_t size)
{
    tjhandle tj = tjInitDecompress();
    int w, h, subsamp, colorspace;
    if (tj && tjDecompressHeader3(tj, jpeg, size, &w, &h, &subsamp, &colorspace) == 0 && w > 0 && h > 0 && w <= 1024 && h <= 1024) {
        img->px = malloc((size_t)w * h * 4);
        // TJPF_BGRA en memoria = 0xAARRGGBB como u32 (little-endian), el formato de gfx.
        if (img->px && tjDecompress2(tj, jpeg, size, (unsigned char *)img->px, w, w * 4, h, TJPF_BGRA, 0) == 0) {
            img->w = w;
            img->h = h;
        } else {
            free(img->px);
            img->px = NULL;
        }
    }
    if (tj)
        tjDestroy(tj);
}

/* A small local diagnostic contains no room, account, title, or icon bytes. */
static void record_icon_status(u64 id, unsigned attempt, unsigned long result,
                               u64 size, const char *decode, const Image *icon, int installed)
{
    const char *path = rc_storage_path_gen(roomcircuit_storage_generation(), RC_LEGACY_PATH("icon-status.log"), RC_V1_PATH("icon-status.log"), RC_CURRENT_PATH("icon-status.log"));
    FILE *f = fopen(path, "a+");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return; }
    if (n > 16384) {
        fclose(f);
        f = fopen(path, "w");
        if (!f) return;
    }
    fprintf(f, "app=%016lX attempt=%u ns=%08lX bytes=%lu icon=%s size=%dx%d installed=%s\n",
            (unsigned long)id, attempt, result, (unsigned long)size, decode,
            icon->w, icon->h, installed < 0 ? "unavailable" : installed ? "yes" : "no");
    fclose(f);
}

static bool init_ns_with_diagnostic(void)
{
    const Result result = nsInitialize();
    if (R_FAILED(result)) {
        const Image empty = {0};
        record_icon_status(0, 0, (unsigned long)result, 0, "ns-init-failed", &empty, -1);
        return false;
    }
    return true;
}

/* id is a candidate ApplicationID; the caller must separately verify LCID. */
static const GameInfo *game_info(u64 id)
{
    if (!id || id == UINT64_MAX || !g_ns_ready) return NULL;
    GameInfo *g = NULL;
    for (int i = 0; i < g_game_count; i++) {
        if (g_games[i].id == id) {
            g = &g_games[i];
            break;
        }
    }
    if (g && ((g->icon.px && g->install_checked) || g->attempts >= 3 ||
              now_ms() < g->retry_after_ms)) return g;
    /* NS uses a shared libnx service: do not race the inventory thread. */
    if (atomic_load(&g_inventory_running)) return g;
    if (!g) {
        const unsigned capacity = sizeof(g_games) / sizeof(g_games[0]);
        if ((unsigned)g_game_count < capacity) {
            g = &g_games[g_game_count++];
        } else {
            g = &g_games[g_game_replace++ % capacity];
            free(g->icon.px);
        }
        memset(g, 0, sizeof(*g));
        g->id = id;
    }
    /* Bounded retries avoid a permanent negative cache without decoding per frame. */
    g->attempts++;
    g->retry_after_ms = now_ms() + (g->attempts == 1 ? 1000 : 5000);
    static NsApplicationControlData control;
    u64 size = 0;
    Result ns_result = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, &control, sizeof(control), &size);
    const char *decode_status = "no-data";
    bool entity_installed = false;
    g->install_checked = R_SUCCEEDED(nsIsAnyApplicationEntityInstalled(id, &entity_installed));
    g->installed = g->install_checked && entity_installed;
    if (R_SUCCEEDED(ns_result) && size >= sizeof(control.nacp) && size <= sizeof(control)) {
        decode_status = g->icon.px ? "cached" : "missing";
        memcpy(g->lcids, control.nacp.local_communication_id, sizeof(g->lcids));
        const int zh_preference[] = { 14, 13, 0, 1 };
        const int es_preference[] = { 6, 5, 0, 1 };
        const int en_preference[] = { 0, 1, 14, 13 };
        const int *preference = g_zh ? zh_preference : (g_es ? es_preference : en_preference);
        char name[0x200];
        nacp_name((const unsigned char *)&control.nacp, sizeof(control.nacp), preference, 4, name, sizeof(name));
        if (name[0])
            game_identity_safe_title((const uint8_t *)name, strlen(name), g->name, sizeof(g->name));
        if (!g->icon.px && size > sizeof(control.nacp)) {
            Image icon = {0};
            decode_icon(&icon, control.icon, size - sizeof(control.nacp));
            if (icon.px) g->icon = icon;
            decode_status = icon.px ? "decoded" : "decode-failed";
        }
    }
    record_icon_status(id, g->attempts, (unsigned long)ns_result, size, decode_status, &g->icon,
                       g->install_checked ? (int)g->installed : -1);
    return g;
}

static void clear_game_cache(void)
{
    for (int i = 0; i < g_game_count; i++) free(g_games[i].icon.px);
    memset(g_games, 0, sizeof(g_games));
    g_game_count = g_game_replace = 0;
}

// --- lista de grupos en un hilo (la consulta al servidor tarda) ---

static Mutex g_list_lock;
static LobbyGroup g_groups[LOBBY_PAGE_SIZE];
static LobbySource g_list_sources[LOBBY_PAGE_SIZE];
static enum LobbyDiscovery g_list_discovery;
static UnifiedPage g_list_unified;
static u32 g_list_request_snapshot;
enum { ListUnavailable = -3, ListConnecting = -2, ListOffline = -1 };
static LobbyPage g_list_page;
static LobbyNodeRoom g_node_rows[LOBBY_PAGE_SIZE];
static bool g_nodes_directory;
static u64 g_node_game_filter;
static u32 g_node_mask=(1u<<NODE_SHARED_CAPACITY)-1;
static uint8_t g_node_region=255;
static u32 g_list_offset, g_list_response_ms;
static u64 g_list_game_id;
static char g_list_provider[33] = "self";
static unsigned g_list_generation;
static bool g_list_enabled;
static bool g_list_changed;
static bool g_quit;
static UEvent g_refresh;

static u64 now_ms(void)
{
    return armTicksToNs(armGetSystemTick()) / 1000000;
}

static void list_thread(void *arg)
{
    (void)arg;
    u64 next_query = 0;
    unsigned failures = 0;
    unsigned last_generation = 0;
    enum LobbyDirectory directory = relay_config_ipv4_valid(RELAY_PROFILE_COMMUNITY_IPV4) ||
        relay_config_ipv4_valid(RELAY_PROFILE_EU_IPV4) ? LobbyDirectoryCommunity : LobbyDirectoryUnified;
    for (;;) {
        mutexLock(&g_list_lock);
        const bool quit = g_quit;
        const bool enabled = g_list_enabled;
        const unsigned generation = g_list_generation;
        const u32 offset = g_list_offset;
        const u32 snapshot = g_list_request_snapshot;
        const u64 game_id = g_list_game_id;
        bool node_directory=g_nodes_directory;
        const u64 node_game=g_node_game_filter;
        u32 node_mask=g_node_mask;
        const uint8_t node_region=g_node_region;
        enum LobbyDiscovery discovery = g_list_discovery;
        char provider[sizeof(g_list_provider)];
        memcpy(provider, g_list_provider, sizeof(provider));
        mutexUnlock(&g_list_lock);
        if (quit)
            return;
        if (room_password_worker_exchange(&g_lanywhere_list) || appointments_worker_exchange(&g_lanywhere_list))
            continue;
        if (!enabled || appointments_active()) {
            waitSingle(waiterForUEvent(&g_refresh), UINT64_MAX);
            continue;
        }
        if (generation != last_generation) {
            last_generation = generation;
            failures = 0;
        }
        const u64 started = now_ms();
        if (started < next_query) {
            waitSingle(waiterForUEvent(&g_refresh), (next_query - started) * 1000000ULL);
            continue;
        }

        /* The native directory refreshes asynchronously; consume its latest
         * snapshot on this worker so newly published nodes reach the lobby. */
        if (node_directory && nodes_ui_refresh_catalog(&g_lanywhere_list))
            node_mask = nodes_ui_region_mask(node_region);

        LobbyGroup groups[LOBBY_PAGE_SIZE] = {0};
        LobbySource sources[LOBBY_PAGE_SIZE] = {{0}};
        UnifiedPage unified = {0};
        LobbyNodeRoom node_rows[LOBBY_PAGE_SIZE]={0};
        LobbyPage page;
        if (game_id) {
            page = lanywhere_list_appointment_rooms(game_id, offset, groups, LOBBY_PAGE_SIZE);
            if (discovery != LobbyDiscoveryLegacy)
                for (s32 i = 0; i < page.count; ++i) memcpy(sources[i], "self", sizeof("self"));
        } else if (node_directory) {
            page=lanywhere_list_nodes((NodeRoomsRequest){.offset=offset,.snapshot=snapshot,.game_id=node_game,.node_mask=node_mask},
                groups,sources,node_rows,&unified,&node_directory);
            if(!node_directory){
                if(offset||snapshot)page=(LobbyPage){.count=-1,.next=LOBBY_PAGE_END,.reserved=UnifiedListStale};
                else page=lanywhere_list_unified((UnifiedRoomsRequest){0},groups,sources,&unified,&discovery,&directory);
            } else discovery=LobbyDiscoveryUnified;
        } else if (discovery == LobbyDiscoveryLegacy) {
            page = lanywhere_list_groups(offset, groups, LOBBY_PAGE_SIZE);
        } else {
            page = lanywhere_list_unified((UnifiedRoomsRequest){.offset = offset, .snapshot = snapshot},
                                         groups, sources, &unified, &discovery, &directory);
        }
        const u64 finished = now_ms();
        const u32 response_ms = page.count >= 0 ? (u32)(finished - started) : 0;
        next_query = finished + MIN_REFRESH_MS;
        mutexLock(&g_list_lock);
        const bool current = lobby_response_is_current(generation, offset, provider, g_list_generation, g_list_offset, g_list_provider);
        if (current) {
            failures = page.count >= 0 ? 0 : failures < 4 ? failures + 1 : 4;
            if (page.count != g_list_page.count || page.next != g_list_page.next || response_ms != g_list_response_ms ||
                page.capabilities != g_list_page.capabilities || page.reserved != g_list_page.reserved ||
                g_list_discovery != discovery || memcmp(&unified, &g_list_unified, sizeof(unified)) != 0 ||
                memcmp(sources, g_list_sources, sizeof(sources)) != 0 ||
                memcmp(groups, g_groups, sizeof(groups)) != 0)
                g_list_changed = true;
            memcpy(g_groups, groups, sizeof(groups));
            memcpy(g_node_rows,node_rows,sizeof(node_rows));
            g_nodes_directory=node_directory;
            memcpy(g_list_sources, sources, sizeof(sources));
            g_list_discovery = discovery;
            g_list_unified = unified;
            g_list_page = page;
            g_list_response_ms = response_ms;
        }
        mutexUnlock(&g_list_lock);
        if (current)
            waitSingle(waiterForUEvent(&g_refresh), lobby_query_delay_ms(page.count, page.reserved, failures) * 1000000ULL);
    }
}

static void restart_group_query(u32 offset, u32 snapshot, bool available, const char *provider, bool set_game, u64 game_id)
{
    mutexLock(&g_list_lock);
    if (provider) snprintf(g_list_provider, sizeof(g_list_provider), "%s", provider);
    if (set_game) g_list_game_id = game_id;
    g_list_generation++;
    g_list_enabled = true;
    g_list_offset = offset;
    g_list_request_snapshot = snapshot;
    g_list_unified = (UnifiedPage){0};
    g_list_page = (LobbyPage){ .count = available ? ListConnecting : ListUnavailable, .next = LOBBY_PAGE_END };
    g_list_response_ms = 0;
    memset(g_groups, 0, sizeof(g_groups));
    memset(g_node_rows,0,sizeof(g_node_rows));
    memset(g_list_sources, 0, sizeof(g_list_sources));
    g_list_changed = true;
    mutexUnlock(&g_list_lock);
    ueventSignal(&g_refresh);
}

static void restart_group_list_on_network(u32 offset, bool available, const char *provider)
{
    restart_group_query(offset, 0, available, provider, true, 0);
}

static void restart_group_list(u32 offset, u32 snapshot, bool available)
{
    restart_group_query(offset, snapshot, available, NULL, false, 0);
}

// --- pantalla ---

#define C_BG_TOP    RGBA(0x00, 0x00, 0x00, 0xFF)
#define C_BG_BOTTOM RGBA(0x00, 0x00, 0x00, 0xFF)
#define C_TEXT      RGBA(0xE8, 0xE8, 0xE8, 0xFF)
#define C_DIM       RGBA(0xAE, 0xAE, 0xAE, 0xFF)
#define C_LINE      RGBA(0x36, 0x36, 0x36, 0xFF)
#define C_CARD      RGBA(0x08, 0x08, 0x08, 0xFF)
#define C_PANEL     RGBA(0x11, 0x11, 0x11, 0xFF)
#define C_ACCENT    RGBA(0x33, 0x33, 0x33, 0xFF)
#define C_OK        RGBA(0xD8, 0xD8, 0xD8, 0xFF)
#define C_WARN      RGBA(0xE5, 0xBA, 0x6E, 0xFF)
#define C_ERR       RGBA(0xEE, 0x8E, 0x88, 0xFF)
#define C_SEAT_OFF  RGBA(0x40, 0x40, 0x40, 0xFF)
#define C_CODE      RGBA(0xD8, 0xD8, 0xD8, 0xFF)

#define MARGIN      40
#define LIST_W      760
#define SIDE_X      (MARGIN + LIST_W + 32)
#define SIDE_W      (GFX_W - SIDE_X - MARGIN)
#define FOOTER_Y    (GFX_H - 72)
#define NETWORK_ROW_TOP 24
#define NETWORK_ROW_STEP 45
#define NETWORK_ROW_HEIGHT 38

// Zonas táctiles del último fotograma: tocar una equivale a pulsar su botón del mando.
typedef struct { int x, y, w, h; u64 button; } TouchZone;
static TouchZone g_zones[48];
#define TOUCH_CARD  (1ULL << 62)  // tocar la tarjeta del grupo n devuelve TOUCH_CARD | n
#define TOUCH_NETWORK_CHOICE (1ULL << 61)
static LobbyRoomId g_rendered_rooms[LOBBY_PAGE_SIZE];
static LobbyRoomId g_touched_room;
static LobbyNodeRoom g_rendered_node_rows[LOBBY_PAGE_SIZE];
static bool g_board_title_animates, g_board_title_chinese;
static unsigned g_board_title_pane;
static u64 g_board_title_id, g_board_title_started;
static LobbyNodeRoom g_touched_node;
static int g_zone_count;

// Botón del mando según la letra que se dibuja.
static u64 button_mask(const char *label)
{
    if (strcmp(label, "→") == 0) return HidNpadButton_Right;
    if (strcmp(label, "←") == 0) return HidNpadButton_Left;
    switch (label[0]) {
    case 'A': return HidNpadButton_A;
    case 'B': return HidNpadButton_B;
    case 'X': return HidNpadButton_X;
    case 'Y': return HidNpadButton_Y;
    case 'R': return HidNpadButton_R;
    case 'L': return HidNpadButton_L;
    case '+': return HidNpadButton_Plus;
    default:  return 0;
    }
}

static void add_zone(int x, int y, int w, int h, u64 button)
{
    if (g_zone_count < (int)(sizeof(g_zones) / sizeof(g_zones[0])))
        g_zones[g_zone_count++] = (TouchZone){ x, y, w, h, button };
}

static u64 touch_action_at(int x, int y)
{
    if (g_guide_open)
        return guide_touch_action(x, y, g_guide_page);
    for (int i = g_zone_count - 1; i >= 0; i--) {
        const TouchZone *z = &g_zones[i];
        if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h)
            return z->button;
    }
    return 0;
}

// Botón del mando que corresponde a un toque nuevo en la pantalla (al levantar el dedo), o 0.
static u64 touch_button(void)
{
    static bool was_down;
    static int last_x, last_y;
    static u64 pressed_action;
    HidTouchScreenState ts = {0};
    bool down = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
    u64 pressed = 0;
    if (down) {
        last_x = ts.touches[0].x;
        last_y = ts.touches[0].y;
        if (!was_down) {
            pressed_action = touch_action_at(last_x, last_y);
            if (pressed_action & TOUCH_CARD) {
                // Hold the identity rendered when the finger went down through any list redraw.
                g_touched_room = g_rendered_rooms[pressed_action & 0xFF];
                g_touched_node = g_rendered_node_rows[pressed_action & 0xFF];
            }
            if (pressed_action & (APPOINTMENT_TOUCH_ROW | APPOINTMENT_TOUCH_JOIN))
                appointments_capture_touch(pressed_action);
        }
    } else if (was_down && touch_action_at(last_x, last_y) == pressed_action) {
        pressed = pressed_action;
    }
    was_down = down;
    return pressed;
}

typedef struct {
    LanywhereStatus st;
    bool private_panel;
    LobbyMatch match;
    LobbyGroup groups[LOBBY_PAGE_SIZE];
    LobbyNodeRoom node_rows[LOBBY_PAGE_SIZE];
    LobbyNodeSelection pinned_node;
    DirectRoomHost direct_host;
    bool node_picker_available, node_directory;
    uint32_t host_node;
    NodeHostInfo host_info;
    bool host_info_valid;
    u32 storage_health;
    uint64_t game_filter;
    uint8_t region_filter;
    LobbySource sources[LOBBY_PAGE_SIZE];
    enum LobbyDiscovery discovery;
    UnifiedPage unified;
    LobbyRoomId selection, pinned;
    LobbyGroup pinned_group;
    s32 count;
    u32 page_index, next_page, response_ms;
    u32 capabilities, list_status;
    RelayConfig config;
    RelayConfigState config_state;
    bool private_room, match_available;
    bool provider_panel, external_confirm, inventory_error;
    int provider_choice;
    u32 host_region, host_regions;
    bool host_region_available;
    char message[128];
    u64 room_notice_until;
    bool room_notice_visible, room_notice_from_session, status_valid;
    u64 appointment_game_id;
    RoomPasswordView password;
    RoomPasswordEdit password_edit;
    int sel;  // grupo elegido en la lista, o -1
} View;

static u32 read_storage_health(Service *service)
{
    u32 health = 0;
    const Result result = serviceDispatchOut(service, RC_STORAGE_HEALTH_IPC, health);
    const u32 known = RC_STORAGE_HEALTH_LEGACY_CHANGED | RC_STORAGE_HEALTH_LEGACY_UNCHECKED |
        RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT | RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED;
    if ((R_FAILED(result) && !(roomcircuit_storage_generation() != 3 && result == MAKERESULT(10, 221))) ||
        (R_SUCCEEDED(result) && (health & ~known))) return RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED;
    return health;
}

static const char *storage_health_text(u32 health)
{
    return ui_text(health & RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT ?
                   "Server settings conflict. Compare config.ini files." :
                   health & RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED ?
                   "Could not check server settings. Try again." :
                   health & RC_STORAGE_HEALTH_LEGACY_CHANGED ?
                   "Older settings changed. Back up and compare." :
                   "Could not check older settings. Back up and compare.");
}

static const char *hosting_node_label(const View *v, char *buffer, size_t size)
{
    if (!v->host_info_valid) return ui_text("Unavailable");
    const char *name = ui_text(v->host_info.name);
    const char *state = v->host_info.status == NodeHostUnavailable ? ui_text("Unavailable") :
        v->host_info.status == NodeHostPending ? ui_text("Checking") : NULL;
    if (state) snprintf(buffer, size, "%s · %s", name, state);
    else snprintf(buffer, size, "%s", name);
    return buffer;
}

static bool consume_inventory_result(View *v)
{
    if (!atomic_exchange(&g_inventory_finished, false)) return false;
    clear_game_cache();
    v->inventory_error = !atomic_load(&g_inventory_success);
    return true;
}

static bool show_matching(const View *v)
{
    return v->match.mode != LobbyManual;
}

static bool external_room_expired(const View *v)
{
    return v->external_confirm && strcmp(v->pinned.provider, REGION_PROVIDER_COMMUNITY) == 0 &&
        (v->match.state == MatchExpired || v->match.state == MatchError);
}

static bool supports_reserved(const View *v)
{
    return v->config_state == RelayConfigReady &&
        (lobby_provider_is_self_relay(v->pinned.provider) ||
         ((v->host_region != RelayRegionCommunity || v->appointment_game_id) &&
          (v->capabilities & LOBBY_CAP_RESERVED_MATCH) &&
          (v->discovery == LobbyDiscoveryUnified || v->appointment_game_id ||
           !lobby_provider_is_lanplay(v->config.provider))));
}

static bool is_self_network(const View *v)
{
    return strcmp(v->config.provider, "self") == 0;
}

static bool is_self_room_context(const View *v)
{
    if (v->pinned.provider[0]) return lobby_provider_is_self_relay(v->pinned.provider);
    return v->discovery == LobbyDiscoveryUnified || v->appointment_game_id || is_self_network(v);
}

static bool has_direct_host(const View *v)
{
    return v->status_valid && v->st.state == StateInRoom && v->st.my_index == 0 && v->direct_host.epoch &&
        v->direct_host.node_id >= 2 && nodes_ui_valid(v->direct_host.node_id) &&
        (v->direct_host.flags & DirectRoomHostActive);
}

static bool owns_direct_room(const View *v, const LobbyNodeRoom *row)
{
    return has_direct_host(v) && row->flags == NodeRoomDirect && row->node_id == v->direct_host.node_id &&
        row->locator.host_ip == v->direct_host.host_ip &&
        memcmp(row->locator.network_id, v->direct_host.network_id, sizeof(row->locator.network_id)) == 0 &&
        memcmp(row->locator.bssid, v->direct_host.bssid, sizeof(row->locator.bssid)) == 0;
}

static bool has_pinned_direct_room(const View *v)
{
    return v->external_confirm && v->pinned_node.node_id >= 2 && v->pinned_node.node_id < NODE_SHARED_CAPACITY && nodes_ui_valid(v->pinned_node.node_id) &&
        v->pinned.game_id && v->pinned.game_id == v->pinned_node.game_id &&
        strcmp(v->pinned.provider, nodes_ui_provider(v->pinned_node.node_id)) == 0 &&
        memcmp(v->pinned.code, v->pinned_node.locator.code, sizeof(v->pinned.code)) == 0;
}

static bool can_share_game_password(const View *v)
{
    if (!v->status_valid || v->config_state != RelayConfigReady || v->provider_panel || v->private_panel ||
        v->private_room || show_matching(v) || v->st.state != StateInRoom || v->st.my_index != 0 ||
        !v->st.game_id || !v->st.code[0]) return false;
    if (v->host_regions & RELAY_REGION_COMMUNITY_ACTIVE)
        return has_direct_host(v) && (v->direct_host.flags & DirectRoomHostCanEdit);
    return is_self_room_context(v) && (v->capabilities & (ROOM_PASSCODE_CAPABILITY | ROOM_ACCESS_CAPABILITY));
}

static LobbyRoomId room_password_target(const View *v)
{
    if (v->config_state != RelayConfigReady || !is_self_room_context(v) ||
        (!v->password.room.game_id && !(v->capabilities & (ROOM_PASSCODE_CAPABILITY | ROOM_ACCESS_CAPABILITY))) ||
        !v->match_available)
        return (LobbyRoomId){0};
    const bool active = v->match.state >= MatchWaitScan && v->match.state <= MatchJoining;
    if (v->match.mode == LobbyExact) {
        const bool retry = ((v->capabilities & ROOM_ACCESS_CAPABILITY) || v->password.locked) &&
            (v->match.state == MatchError || v->match.state == MatchExpired);
        return active || retry ? v->pinned : (LobbyRoomId){0};
    }
    LobbyRoomId target = {0};
    if (v->match.mode == LobbyQuick && active && v->match.state >= MatchAssigned && v->match.room[0]) {
        target.game_id = v->match.game_id;
        memcpy(target.code, v->match.room, sizeof(target.code));
        if (v->host_region_available && v->host_region == RelayRegionEurope)
            memcpy(target.provider, REGION_PROVIDER_EU, sizeof(REGION_PROVIDER_EU));
    }
    return target;
}

static bool update_room_password(View *v, u64 now)
{
    if (has_pinned_direct_room(v) && !external_room_expired(v) &&
        (v->pinned_group.room_access == RoomAccessShared || v->pinned_group.room_access == RoomAccessHidden))
        return room_password_tick_direct(&v->password, v->pinned, &v->pinned_node,
                                         v->pinned_group.room_access == RoomAccessHidden, now);
    const bool stale = v->match.state == MatchExpired || v->match.state == MatchError;
    const bool was_unlocked = v->password.unlocked;
    if (stale) v->password.unlocked = false;
    const bool known_locked = v->match.mode == LobbyExact && v->pinned_group.room_access == RoomAccessHidden;
    const bool changed = room_password_tick(&v->password, room_password_target(v), known_locked, now);
    return changed || (was_unlocked && stale);
}

static bool can_enter_game_password(const View *v)
{
    return v->match_available && v->match.mode == LobbyExact && v->pinned.game_id &&
        v->password.locked && !v->password.unlocked && !v->password.pending &&
        lobby_room_equal(v->pinned, v->password.room);
}

static bool room_password_allows_launch(const View *v)
{
    /* A direct room's password is checked by the game. The lobby only displays it. */
    if (has_pinned_direct_room(v)) return true;
    if (v->password.pending || (v->password.locked && !v->password.unlocked)) return false;
    if (v->pinned_group.room_access != RoomAccessHidden) return true;
    return v->pinned.game_id && lobby_room_equal(v->pinned, v->password.room);
}

static const char *match_state_text(u8 state)
{
    switch (state) {
    case MatchWaitScan: return TR("Esperando la búsqueda del juego", "Waiting for in-game search");
    case MatchSearching: return TR("Buscando una sala compatible", "Searching for a compatible room");
    case MatchAssigned: return TR("Sala asignada", "Room assigned");
    case MatchJoining: return TR("Entrando desde el juego…", "Joining from the game…");
    case MatchJoined: return TR("Conectado a la sala", "Joined the room");
    case MatchExpired: return TR("Búsqueda caducada", "Search expired");
    case MatchCancelled: return TR("Búsqueda cancelada", "Search cancelled");
    case MatchError: return TR("Error de búsqueda", "Search error");
    default: return TR("Listo", "Ready");
    }
}

// Capturas sin mando: si aparece sdmc:/lanywhere/captura.flag (por ejemplo, copiado por FTP), el siguiente
// fotograma se guarda como sdmc:/lanywhere/captura-N.png y el archivo se borra.
#define CAPTURE_FLAG rc_storage_path_gen(roomcircuit_storage_generation(), RC_LEGACY_PATH("captura.flag"), RC_V1_PATH("captura.flag"), RC_CURRENT_PATH("captura.flag"))
static bool g_capture_pending;

static bool capture_requested(void)
{
    struct stat st;
    return stat(CAPTURE_FLAG, &st) == 0;
}

static void save_capture_if_requested(void)
{
    if (!g_capture_pending)
        return;
    g_capture_pending = false;
    char path[64];
    struct stat st;
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof(path), "%s/captura-%d.png", RELAY_CONFIG_DIR, n);
        if (stat(path, &st) == 0) continue;
        if (errno != ENOENT) return;
        if (gfx_save_png(path)) remove(CAPTURE_FLAG);
        return;
    }
}

static const char *list_error_title(u32 status)
{
    switch (status) {
    case ListStatusBusy: return TR("Red ocupada", "Network busy");
    case ListStatusUnavailable: return TR("Red no disponible", "Network unavailable");
    case ListStatusTimeout: return TR("La red tarda en responder", "Network timed out");
    case ListStatusProtocol: return TR("Respuesta no compatible", "Unsupported response");
    case ListStatusConfig: return TR("Revisa los ajustes de red", "Check network settings");
    case ListStatusPaused: return TR("Servidor pausado", "Lobby paused");
    case ListStatusUpdate: return TR("Actualización necesaria", "Update required");
    case ListStatusNetwork: return TR("Reconectando…", "Retrying connection…");
    default: return TR("Sin conexión", "Offline");
    }
}

static void status_pill(const View *v)
{
    const char *text = TR("Servidor disponible", "Lobby online");
    u32 dot = C_OK;
    if (v->config_state == RelayConfigFileError || v->config_state == RelayConfigInvalid) {
        text = v->config_state == RelayConfigFileError ?
            TR("No se puede leer la red", "Cannot read network settings") :
            TR("Ajustes de red no válidos", "Invalid network settings");
        dot = C_ERR;
    } else if (!RELAY_PROFILE_FIXED && v->config_state != RelayConfigReady) {
        text = v->config_state == RelayConfigNotSet ? TR("Configura el servidor", "Set your server") :
               TR("Revisa la configuración", "Check server settings");
        dot = C_WARN;
    } else if (v->count == ListConnecting) {
        text = TR("Conectando…", "Connecting…");
        dot = C_WARN;
    } else if (v->count == ListUnavailable) {
        text = TR("Lista no disponible", "List unavailable");
        dot = C_ERR;
    } else if (v->count < 0) {
        text = list_error_title(v->list_status);
        dot = C_ERR;
    }
    int tw = gfx_text_width(22, text);
    int w = 18 + 14 + 10 + tw + 20, x = 1024 - w, y = 42;
    gfx_round_rect(x, y, w, 40, 20, C_CARD);
    gfx_round_rect(x + 18, y + 13, 14, 14, 7, dot);
    gfx_text(x + 42, y + 8, 22, C_TEXT, text);
}

static void seats(int x, int y, int players, int max)
{
    if (max <= 0 || max > 8)
        max = 8;
    for (int i = 0; i < max; i++)
        gfx_round_rect(x + i * 22, y, 16, 16, 8, i < players ? C_OK : C_SEAT_OFF);
}

static const GameInfo *local_game_info(const LobbyGroup *group)
{
    if (!group || !group->metadata_flags || !group->application_id) return NULL;
    const GameInfo *info = game_info(group->application_id);
    return info && info->installed && lobby_application_matches(group, info->id, info->lcids) ? info : NULL;
}

static CatalogMatch room_catalog_match(const LobbyGroup *group)
{
    if (!group) return (CatalogMatch){0};
    const bool host_id = (group->metadata_flags & LOBBY_META_HOST) && !(group->metadata_flags & LOBBY_META_LOCAL);
    return game_catalog_identify(&g_catalog, host_id ? group->application_id : 0, group->game_id);
}

static bool game_info_has_lcid(const GameInfo *info, u64 lcid)
{
    if (!info || !lcid || lcid == UINT64_MAX) return false;
    for (size_t i = 0; i < sizeof(info->lcids) / sizeof(info->lcids[0]); i++)
        if (info->lcids[i] == lcid) return true;
    return false;
}

static const char *room_title(const LobbyGroup *group, char *fallback, size_t capacity)
{
    const GameInfo *local = local_game_info(group);
    const char *name = local && local->name[0] ? local->name : NULL;
    if (!name && group && group->metadata_flags) {
        const char *preferred = g_zh ? group->title_zh : group->title_en;
        const char *alternate = g_zh ? group->title_en : group->title_zh;
        name = preferred[0] ? preferred : alternate[0] ? alternate : NULL;
    }
    if (name) return name;
    const CatalogMatch catalog = room_catalog_match(group);
    if (catalog.app_id && group && !group->metadata_flags) {
        const GameInfo *info = game_info(catalog.app_id);
        if (info && info->installed && info->name[0] && game_info_has_lcid(info, group->game_id))
            return info->name;
    }
    const char *catalog_name = game_catalog_name(&catalog, g_zh);
    if (catalog_name) return catalog_name;
    if (!group || !group->game_id || group->game_id == UINT64_MAX)
        return TR("No se proporcionó información del juego", "Game info not provided");
    snprintf(fallback, capacity, TR("ID del juego %016lX", "Game ID %016lX"), (unsigned long)group->game_id);
    return fallback;
}

static const char *direct_host_title(const View *v)
{
    const char *preferred = g_zh ? v->direct_host.title_zh : v->direct_host.title_en;
    const char *alternate = g_zh ? v->direct_host.title_en : v->direct_host.title_zh;
    if (preferred[0]) return preferred;
    if (alternate[0]) return alternate;
    const GameInfo *installed = v->direct_host.application_id ? game_info(v->direct_host.application_id) : NULL;
    return installed && installed->installed && installed->name[0] ? installed->name : NULL;
}

/* A host-declared ApplicationID is a display hint only, never a launch check. */
static const GameInfo *display_game_icon(const LobbyGroup *group)
{
    if (!group) return NULL;
    if (group->application_id && (group->metadata_flags & LOBBY_META_HOST) && !(group->metadata_flags & LOBBY_META_LOCAL))
        return game_info(group->application_id);
    if (group->application_id && (group->metadata_flags & LOBBY_META_LOCAL)) {
        const GameInfo *info = game_info(group->application_id);
        return info && lobby_application_matches(group, info->id, info->lcids) ? info : NULL;
    }
    const CatalogMatch catalog = room_catalog_match(group);
    const GameInfo *info = catalog.app_id ? game_info(catalog.app_id) : NULL;
    return info && info->installed && game_info_has_lcid(info, group->game_id) ? info : NULL;
}

static const char *room_title_note(const LobbyGroup *group)
{
    const CatalogMatch catalog = room_catalog_match(group);
    const bool host = (group->metadata_flags & LOBBY_META_HOST) && !(group->metadata_flags & LOBBY_META_LOCAL);
    const bool host_name = host && (group->title_en[0] || group->title_zh[0]);
    const bool local_candidate = (group->metadata_flags & LOBBY_META_LOCAL) && (group->title_en[0] || group->title_zh[0]);
    const u64 app_id = host && group->application_id ? group->application_id : catalog.app_id;
    const GameInfo *info = !local_candidate && app_id ? game_info(app_id) : NULL;
    if (info && info->install_checked && !info->installed)
        return TR("No instalado", "Not installed");
    if (local_candidate) return TR("Coincidencia local", "Local match");
    if (!host_name && !local_candidate && catalog.kind == CatalogShared)
        return TR("ID compartido", "Shared game ID");
    if (!host_name && !local_candidate && (catalog.kind == CatalogEqualId || catalog.kind == CatalogCommunication) &&
        !(info && info->installed && game_info_has_lcid(info, group->game_id)))
        return TR("Coincidencia del catálogo", "Catalog match");
    return NULL;
}

static const char *provider_label(int index)
{
    const char *provider = lobby_provider_id(index);
    if (provider) {
        for (unsigned id = 0; id < NODE_CATALOG_COUNT; id++) {
            if (strcmp(provider, LanywhereNodes[id].provider) == 0) {
                return nodes_ui_name(id);
            }
        }
    }
    return TR("Red desconocida", "Unknown network");
}

static const char *region_label(u32 region)
{
    if (region == RelayRegionEurope) return ui_text("Europe");
    if (region == RelayRegionCommunity) return ui_text("Community · LANywhere");
    return ui_text("Main server");
}

static void group_card(int x, int y, int w, const LobbyGroup *g, const char *source, bool selected)
{
    const int h = 60;
    if (selected) {
        gfx_round_rect(x - 4, y - 4, w + 8, h + 8, 20, C_ACCENT);
        gfx_round_rect(x, y, w, h, 16, C_BG_TOP);
    }
    gfx_round_rect(x, y, w, h, 16, C_CARD);
    const GameInfo *info = display_game_icon(g);
    char fallback[80];
    if (info && info->icon.px) {
        gfx_image(&info->icon, x + 12, y + 9, 48, 48);
    } else {
        gfx_round_rect(x + 12, y + 9, 48, 48, 10, RGBA(0x2C, 0x2C, 0x2C, 0xFF));
        if (room_catalog_match(g).kind != CatalogNone) {
            gfx_round_rect(x + 20, y + 24, 32, 18, 6, C_DIM);
            gfx_round_rect(x + 25, y + 32, 10, 2, 1, C_BG_TOP);
            gfx_round_rect(x + 29, y + 28, 2, 10, 1, C_BG_TOP);
            gfx_round_rect(x + 42, y + 31, 4, 4, 2, C_BG_TOP);
        } else {
            gfx_text(x + 29, y + 16, 24, C_DIM, "?");
        }
    }
    int tx = x + 78, right = x + w - 24;
    const char *note = g->room_access == RoomAccessHidden ? ui_text("Password required") : room_title_note(g);
    char details[160] = {0};
    if (source && source[0]) {
        unsigned custom = 0;
        char trailing = 0;
        const bool shared_custom = sscanf(source, "custom-%u%c", &custom, &trailing) == 1 &&
            custom >= NODE_CATALOG_COUNT && custom < NODE_SHARED_CAPACITY && nodes_ui_valid(custom);
        const char *label = shared_custom ? nodes_ui_name(custom) :
            strcmp(source, REGION_PROVIDER_COMMUNITY) == 0 ? region_label(RelayRegionCommunity) :
            strcmp(source, REGION_PROVIDER_EU) == 0 ? region_label(RelayRegionEurope) : provider_label(lobby_provider_index(source));
        const char *region = shared_custom ? nodes_ui_location(custom) : NULL;
        for (unsigned id = 0; id < NODE_CATALOG_COUNT; id++) {
            if (strcmp(source, LanywhereNodes[id].provider) == 0) {
                region = nodes_ui_location(id);
                break;
            }
        }
        snprintf(details, sizeof(details), "%s%s%s", label, region ? " · " : "", region ? region : "");
    }
    if (note) {
        const size_t used = strlen(details);
        snprintf(details + used, sizeof(details) - used, "%s%s", used ? " · " : "", note);
    }
    gfx_text_fit(tx, y + (details[0] ? 9 : 18), 23, C_TEXT, room_title(g, fallback, sizeof(fallback)), right - 190 - tx);
    if (details[0])
        gfx_text_fit(tx, y + 36, 17, C_DIM, details, right - 190 - tx);

    char count[16];
    snprintf(count, sizeof(count), "%u/%u", g->players, g->max_players);
    int cw = gfx_text_width(23, count);
    gfx_text(right - cw, y + 8, 23, C_TEXT, count);
    if (lobby_room_full(g)) {
        const char *full = ui_text("Full");
        gfx_text(right - cw - 16 - gfx_text_width(18, full), y + 12, 18, C_WARN, full);
    }
    seats(right - 8 * 22 + 6, y + 36, g->players, g->max_players);
}

static void draw_room_password_panel(const View *v, int x, int panel_y)
{
    const bool has_password = v->password.value[0] != '\0';
    const bool password_panel = has_password || v->password.locked || v->password.pending || v->password.failed;
    if (password_panel) {
        const int panel_h = has_password ? 132 : v->password.locked ? 94 : 64;
        gfx_round_rect(x, panel_y, LIST_W, panel_h, 18, C_PANEL);
        if (has_password) {
            char code[2 * ROOM_PASSCODE_MAX + 1] = {0};
            char *out = code;
            bool spaces = false;
            for (const char *c = v->password.value; *c; ++c) {
                if (*c == ' ') {
                    memcpy(out, "·", 2);
                    out += 2;
                    spaces = true;
                } else {
                    *out++ = *c;
                }
            }
            gfx_text(x + 28, panel_y + 12, 23, C_TEXT, ui_text("Game password"));
            gfx_text_fit(x + 28, panel_y + 45, 28, C_CODE, code, LIST_W - 56);
            gfx_text(x + 28, panel_y + 96, 20, C_DIM,
                spaces ? ui_text("Enter it in the game. · means space.") : ui_text("Enter it in the game."));
        } else if (v->password.locked) {
            gfx_text(x + 28, panel_y + 13, 24, C_WARN, v->password.unlocked ? ui_text("Room unlocked") : ui_text("Password required"));
            const char *hint = v->password.unlocked ? ui_text("Enter the same password in the game if asked.") :
                v->password.pending ? ui_text("Checking game password…") : ui_text("Ask the host for the password.");
            gfx_text_fit(x + 28, panel_y + 53, 20, C_DIM, hint, LIST_W - 56);
        } else {
            const char *message = v->password.pending ? ui_text("Checking game password…") : ui_text("Game password unavailable.");
            gfx_text_fit(x + 28, panel_y + 18, 21, C_DIM, message, LIST_W - 56);
        }
    }
}

static void matching_column(const View *v)
{
    const int x = MARGIN, y = 174;
    const bool quick = v->match.mode == LobbyQuick;
    gfx_text(x, 128, 28, C_TEXT, quick ? TR("Búsqueda rápida", "Quick match") : TR("Grupo elegido", "Selected room"));
    gfx_round_rect(x, y, LIST_W, 264, 18, C_PANEL);
    gfx_text(x + 28, y + 28, 30, C_TEXT, match_state_text(v->match.state));
    if (v->match.game_id) {
        const LobbyGroup unknown = { .game_id = v->match.game_id };
        const LobbyGroup *group = v->pinned_group.game_id == v->match.game_id ? &v->pinned_group : &unknown;
        char fallback[80];
        gfx_text_fit(x + 28, y + 78, 22, C_DIM, room_title(group, fallback, sizeof(fallback)), LIST_W - 56);
        if (group == &v->pinned_group) {
            const char *game_status = room_title_note(group);
            if (game_status) {
                const int status_width = gfx_text_width(17, game_status);
                gfx_text(x + LIST_W - 28 - status_width, y + 114, 17, C_DIM, game_status);
            }
        }
    }
    const char *code = v->match.room[0] ? v->match.room : (quick ? "" : v->pinned.code);
    if (code[0]) {
        char room[64];
        snprintf(room, sizeof(room), TR("Sala %.6s", "Room %.6s"), code);
        gfx_text(x + 28, y + 113, 24, C_CODE, room);
        if ((!quick && strcmp(v->pinned.provider, REGION_PROVIDER_EU) == 0) ||
            (quick && v->host_region_available && v->host_region == RelayRegionEurope))
            gfx_text(x + 195, y + 117, 18, C_DIM, region_label(RelayRegionEurope));
    }
    if (v->match.target) {
        char count[64];
        snprintf(count, sizeof(count), TR("Plazas ocupadas: %u/%u", "Seats taken: %u/%u"), v->match.players, v->match.target);
        gfx_text(x + 280, y + 117, 20, C_DIM, count);
    }
    if (v->match.ttl_seconds) {
        char ttl[64];
        snprintf(ttl, sizeof(ttl), TR("Caduca en %u s", "Expires in %u s"), v->match.ttl_seconds);
        gfx_text(x + 28, y + 150, 18, C_DIM, ttl);
    }
    if (v->match.error[0]) {
        gfx_text_fit(x + 28, y + 178, 20, C_ERR, tr_error(v->match.error), LIST_W - 56);
    }
    if ((!v->password.locked || v->password.unlocked) &&
        (v->match.state == MatchWaitScan || v->match.state == MatchSearching || v->match.state == MatchAssigned)) {
        gfx_text_fit(x + 28, y + 212, 20, C_DIM,
            TR("Abre el juego, entra en inalámbrico local y elige Buscar grupo.", "Open the game, enter Local Wireless and choose Search for groups."), LIST_W - 56);
    }
    draw_room_password_panel(v, x, y + 276);
}

static const char *network_offline_hint(const char *provider)
{
    if (!RELAY_PROFILE_FIXED)
        return TR("Comprueba internet, la dirección y el puerto del servidor.", "Check the console network and the relay address and port.");
    if (provider && provider[0] && strcmp(provider, "self") != 0)
        return TR("Esta red no está disponible. Elige otra con LS.", "This network is unavailable. Choose another with LS.");
    return TR("Comprueba la conexión a internet de la consola.", "Check the console's internet connection.");
}

static const char *list_error_hint(u32 status, const char *provider)
{
    switch (status) {
    case ListStatusNetwork:
        return TR("La lista se actualiza cuando vuelve la red.", "The list refreshes when the network returns.");
    case ListStatusBusy:
    case ListStatusTimeout:
        return provider ?
            TR("Vuelve a intentarlo o cambia de red con LS.", "Try again, or choose another network with LS.") :
            ui_text("The lobby will retry automatically.");
    case ListStatusProtocol:
    case ListStatusUpdate:
        return TR("Actualiza la app y el complemento juntos.", "Update the app and plugin together.");
    case ListStatusConfig:
        return TR("Revisa los ajustes de red en la tarjeta SD.", "Check the network settings file on the SD card.");
    case ListStatusPaused:
        return TR("Espera a que el servidor vuelva a estar disponible.", "Wait for the relay to become available.");
    default: return network_offline_hint(provider);
    }
}

static void groups_column(const View *v)
{
    int x = MARGIN, y = 128;
    if (v->external_confirm && (has_pinned_direct_room(v) || strcmp(v->pinned.provider, REGION_PROVIDER_COMMUNITY) == 0)) {
        gfx_text(x, y, 28, C_TEXT, ui_text("Selected room"));
        group_card(x, y + 52, LIST_W, &v->pinned_group, v->pinned.provider, false);
        if (has_pinned_direct_room(v)) draw_room_password_panel(v, x, y + 130);
        return;
    }
    if (show_matching(v)) {
        matching_column(v);
        return;
    }
    gfx_text(x, y, 28, C_TEXT, v->appointment_game_id ? ui_text("Game rooms") :
        v->discovery == LobbyDiscoveryUnified ? ui_text("All rooms") : TR("Grupos abiertos ahora", "Public rooms"));
    if (v->config_state == RelayConfigReady && (v->page_index || v->next_page != LOBBY_PAGE_END)) {
        char page[32];
        snprintf(page, sizeof(page), TR("Página %u", "Page %u"), v->page_index + 1);
        gfx_text(x + 359, y + 7, 19, C_DIM, page);
        if (v->page_index > 0) {
            int w = gfx_button(x + 470, y, "ZL", TR("Anterior", "Prev"));
            add_zone(x + 464, y - 4, w + 12, 40, HidNpadButton_ZL);
        }
        if (v->next_page != LOBBY_PAGE_END) {
            int w = gfx_button(x + 610, y, "ZR", TR("Siguiente", "Next"));
            add_zone(x + 604, y - 4, w + 12, 40, HidNpadButton_ZR);
        }
    }
    y += 52;

    if (v->config_state == RelayConfigFileError || v->config_state == RelayConfigInvalid) {
        gfx_round_rect(x, y, LIST_W, 170, 18, C_PANEL);
        gfx_text(x + 28, y + 30, 26, C_TEXT,
                 v->config_state == RelayConfigFileError ? TR("No se puede leer la red", "Cannot read network settings") :
                 TR("Ajustes de red no válidos", "Invalid network settings"));
        gfx_text_fit(x + 28, y + 76, 20, C_DIM,
                     TR("Revisa los ajustes de red en la tarjeta SD.", "Check the network settings file on the SD card."), LIST_W - 56);
        return;
    }
    if (!RELAY_PROFILE_FIXED && v->config_state != RelayConfigReady) {
        gfx_round_rect(x, y, LIST_W, 170, 18, C_PANEL);
        gfx_text(x + 28, y + 30, 26, C_TEXT, TR("Configura tu servidor", "Set up your relay server"));
        const char *hint = v->config_state == RelayConfigFileError ?
            TR("No se pudo leer config.ini en la tarjeta SD.", "Could not read config.ini on the SD card.") :
            v->config_state == RelayConfigInvalid ?
            TR("La dirección IPv4 o el puerto guardados no son válidos.", "The saved IPv4 address or port is invalid.") :
            TR("Pulsa R o toca el panel para introducir la IPv4 y el puerto.", "Press R or tap the server panel to enter an IPv4 address and port.");
        gfx_text_fit(x + 28, y + 76, 20, C_DIM, hint, LIST_W - 56);
        return;
    }
    if (v->count == ListConnecting) {
        gfx_text(x, y + 10, 22, C_DIM, TR("Buscando grupos…", "Looking for groups…"));
        return;
    }
    if (v->count == ListUnavailable) {
        gfx_text(x, y + 10, 22, C_DIM, TR("Vuelve a abrir la app para consultar la lista.", "Reopen the app to refresh the group list."));
        return;
    }
    if (v->count < 0) {
        gfx_round_rect(x, y, LIST_W, 170, 18, C_PANEL);
        gfx_text(x + 28, y + 30, 26, C_TEXT, list_error_title(v->list_status));
        gfx_text_fit(x + 28, y + 76, 20, C_DIM,
                     list_error_hint(v->list_status, v->discovery == LobbyDiscoveryLegacy ? v->config.provider : NULL), LIST_W - 56);
        return;
    }
    if (v->count == 0) {
        gfx_round_rect(x, y, LIST_W, 200, 18, C_PANEL);
        if (v->page_index) {
            gfx_text(x + 28, y + 30, 26, C_TEXT, TR("Esta página se quedó vacía", "This page is now empty"));
            return;
        }
        if (v->discovery == LobbyDiscoveryLegacy && !is_self_network(v)) {
            gfx_text(x + 28, y + 30, 26, C_TEXT, TR("No se encontraron salas compatibles", "No compatible rooms found"));
            gfx_text_fit(x + 28, y + 82, 22, C_DIM,
                TR("Usa la misma red y el modo inalámbrico local.", "Use the same network and Local Wireless mode."), LIST_W - 56);
            return;
        }
        gfx_text(x + 28, y + 30, 26, C_TEXT, TR("Todavía no hay grupos abiertos", "No open groups yet"));
        gfx_text_fit(x + 28, y + 82, 22, C_DIM,
            TR("Abre el juego > inalámbrico local > crear grupo.", "Game > Local Wireless > Create room."), LIST_W - 56);
        return;
    }
    for (int i = 0; i < v->count; i++) {
        group_card(x, y + i * 68, LIST_W, &v->groups[i], v->sources[i], v->sel == i);
        g_rendered_rooms[i] = lobby_room_id_on(&v->groups[i], v->sources[i]);
        g_rendered_node_rows[i]=v->node_rows[i];
        add_zone(x, y + i * 68, LIST_W, 60, TOUCH_CARD | (u64)i);
    }
}

static bool can_change_visibility(const View *v)
{
    return !v->node_picker_available && v->discovery == LobbyDiscoveryLegacy && v->status_valid && v->config_state == RelayConfigReady && !is_self_network(v) &&
        !v->private_room && !v->private_panel && !v->provider_panel && !v->external_confirm && !show_matching(v) &&
        v->st.state != StateInRoom && v->st.state != StateConnecting;
}

static bool can_change_provider(const View *v)
{
    return v->discovery == LobbyDiscoveryLegacy && v->status_valid && v->config_state == RelayConfigReady && !v->private_room && !v->private_panel && !v->external_confirm && !show_matching(v) && !v->appointment_game_id &&
           (v->st.state == StateIdle || v->st.state == StateError);
}

static bool can_change_host_region(const View *v)
{
    return (v->node_picker_available || v->host_region_available) &&
        (v->node_picker_available || (v->host_regions & ((1u << RelayRegionEurope) | (1u << RelayRegionCommunity))) ||
         v->host_region != RelayRegionDefault) &&
        v->status_valid && v->match_available && v->config_state == RelayConfigReady &&
        !v->appointment_game_id && !v->provider_panel && !v->private_panel && !v->private_room &&
        !v->external_confirm && !show_matching(v) &&
        (v->st.state == StateIdle || v->st.state == StateError);
}

static bool current_host_line_available(const View *v)
{
    return v->node_picker_available || v->host_region == RelayRegionDefault || (v->host_regions & (1u << v->host_region)) != 0;
}

static u32 next_host_region(const View *v)
{
    if (v->host_region == RelayRegionDefault && (v->host_regions & (1u << RelayRegionCommunity)))
        return RelayRegionCommunity;
    if (v->host_region != RelayRegionEurope && (v->host_regions & (1u << RelayRegionEurope)))
        return RelayRegionEurope;
    return RelayRegionDefault;
}

static bool appointment_entry_available(const View *v)
{
    return !v->appointment_game_id && v->config_state == RelayConfigReady && !v->provider_panel &&
        !v->private_panel && !v->private_room && !v->external_confirm && !show_matching(v);
}

static void open_appointment_rooms(View *v, u64 game_id, bool worker_available)
{
    if (v->discovery == LobbyDiscoveryLegacy && !is_self_network(v)) {
        snprintf(v->message, sizeof(v->message), "%s", ui_text("Press LS and choose Main server to find these rooms."));
        return;
    }
    v->appointment_game_id = game_id;
    v->page_index = 0;
    v->next_page = LOBBY_PAGE_END;
    v->selection = (LobbyRoomId){0};
    v->sel = -1;
    v->count = worker_available ? ListConnecting : ListUnavailable;
    restart_group_query(0, 0, worker_available, NULL, true, game_id);
}

static bool room_board_available(const View *v)
{
    return v->node_directory && appointment_entry_available(v);
}

static bool room_filters_requested(const View *v, u64 down, u64 held)
{
    const u64 shortcut = HidNpadButton_ZL | HidNpadButton_ZR;
    return room_board_available(v) &&
        ((down & TOUCH_NODE_FILTER) || ((held & shortcut) == shortcut && (down & shortcut)));
}

static bool lobby_guide_available(const View *v)
{
    return !v->provider_panel && !v->private_panel && !v->private_room &&
        !v->external_confirm && !v->password_edit.active;
}

static bool guide_input(const View *v, u64 down)
{
    const bool in_appointments = appointments_active();
    const bool shortcut = (down & HidNpadButton_Right) &&
        (in_appointments ? appointments_list_active() : lobby_guide_available(v));
#if RELAY_PROFILE_FIXED
    const bool legacy_shortcut = !in_appointments && !v->provider_panel && (down & HidNpadButton_R);
#else
    const bool legacy_shortcut = false;
#endif
    if (!g_guide_open) {
        if (!shortcut && !legacy_shortcut) return false;
        g_guide_page = in_appointments ? GuideAppointmentsPage : 0;
        g_guide_open = true;
        return true;
    }
    if ((down & HidNpadButton_ZL) && g_guide_page > 0) g_guide_page--;
    if ((down & HidNpadButton_ZR) && g_guide_page + 1 < guide_page_count()) g_guide_page++;
    if ((down & HidNpadButton_B) || legacy_shortcut) g_guide_open = false;
    return true;
}


static void provider_column(const View *v)
{
    int x = MARGIN, y = 174;
    gfx_text(x, 128, 28, C_TEXT, TR("Red", "Network"));
    gfx_round_rect(x, y, LIST_W, 400, 18, C_PANEL);
    const int column_width = (LIST_W - 60) / 2;
    for (int i = 0; i < LOBBY_PROVIDER_COUNT; ++i) {
        int row = y + NETWORK_ROW_TOP + (i / 2) * NETWORK_ROW_STEP;
        int column = x + 22 + (i % 2) * (column_width + 16);
        gfx_round_rect(column, row, column_width, NETWORK_ROW_HEIGHT, 10, i == v->provider_choice ? C_ACCENT : C_CARD);
        const bool selected = strcmp(v->config.provider, lobby_provider_id(i)) == 0;
        gfx_text_fit(column + 14, row + 9, 18, C_TEXT, provider_label(i), column_width - (selected ? 100 : 28));
        if (selected) gfx_text(column + column_width - 84, row + 10, 16, C_TEXT, TR("Actual", "Current"));
        add_zone(column, row, column_width, NETWORK_ROW_HEIGHT, TOUCH_NETWORK_CHOICE | (u64)i);
    }
    gfx_text_fit(x + 28, y + 345, 18, C_DIM,
                 TR("* Experimental: salas en el juego, sin códigos.", "* Experimental: in-game rooms, no invite codes."), LIST_W - 56);
}

static void private_column(const View *v)
{
    int x = MARGIN, y = 174;
    gfx_text(x, 128, 28, C_TEXT, TR("Sala de amigos", "Friends room"));
    gfx_round_rect(x, y, LIST_W, 310, 18, C_PANEL);
    if (lobby_provider_is_lanplay(v->config.provider)) {
        gfx_text_fit(x + 28, y + 60, 22, C_DIM, TR("Esta red usa salas dentro del juego.", "This network uses in-game rooms."), LIST_W - 56);
        return;
    }
    if (v->private_room) {
        gfx_text(x + 28, y + 28, 23, C_DIM, TR("Código", "Invite code"));
        gfx_text(x + 28, y + 78, 56, C_CODE, v->st.code);
        char count[64];
        snprintf(count, sizeof(count), TR("%u jugadores", "%u players"), v->st.member_count);
        gfx_text(x + 28, y + 156, 24, C_TEXT, count);
        gfx_text_fit(x + 28, y + 222, 22, C_DIM,
            TR("Abre el juego > inalámbrico local.", "Now open the game > Local Wireless."), LIST_W - 56);
    } else {
        int w = gfx_button(x + 28, y + 44, "A", TR("Crear sala privada", "Create private room"));
        add_zone(x + 20, y + 30, w + 32, 60, HidNpadButton_A);
        w = gfx_button(x + 28, y + 124, "X", TR("Introducir código", "Enter invite code"));
        add_zone(x + 20, y + 110, w + 32, 60, HidNpadButton_X);
    }
}

static void side_column(const View *v)
{
    int x = SIDE_X, y = 128;
#if !RELAY_PROFILE_FIXED
    if (!v->appointment_game_id) {
        gfx_round_rect(x, y, SIDE_W, 90, 18, C_PANEL);
        gfx_button(x + 20, y + 18, "R", TR("Servidor", "Edit server"));
        add_zone(x, y, SIDE_W, 90, HidNpadButton_R);
        y += 104;
    }
#endif
    if (v->discovery == LobbyDiscoveryLegacy && v->config_state == RelayConfigReady && !is_self_network(v) &&
        !v->private_panel && !v->private_room && !show_matching(v)) {
        gfx_round_rect(x, y, SIDE_W, 116, 18, C_PANEL);
        gfx_text(x + 20, y + 18, 24, C_TEXT, TR("Crear en el juego", "Game room visibility"));
        const char *visibility = v->private_room ? TR("Solo amigos", "Friends only") :
            (v->node_picker_available || v->config.public_groups) ? TR("Público", "Public") : TR("Solo local", "Local only");
        gfx_text(x + 20, y + 65, 24, C_CODE, visibility);
        if (can_change_visibility(v)) {
            gfx_button(x + SIDE_W - 82, y + 62, "X", "");
            add_zone(x, y, SIDE_W, 116, HidNpadButton_X);
        }
        y += 132;
    }
    if (can_change_host_region(v)) {
        gfx_round_rect(x, y, SIDE_W, 94, 18, C_PANEL);
        gfx_text(x + 20, y + 12, 24, C_TEXT, ui_text("Hosting line"));
        const char *change = ui_text("Change");
        const int change_x = x + SIDE_W - 20 - gfx_button_width("Y", change);
        const int name_width = change_x - (x + 36);
        if (current_host_line_available(v)) {
            char node_label[96];
            gfx_text_fit(x + 20, y + 49, 24, C_CODE, v->node_picker_available ?
                         hosting_node_label(v, node_label, sizeof(node_label)) : region_label(v->host_region),
                         name_width);
        } else {
            char label[96];
            snprintf(label, sizeof(label), ui_text("%s unavailable"), region_label(v->host_region));
            gfx_text_fit(x + 20, y + 49, 19, C_WARN, label, name_width);
        }
        gfx_button(change_x, y + 44, "Y", change);
        add_zone(x, y, SIDE_W, 94, HidNpadButton_Y);
        y += 110;
    }
    if (can_share_game_password(v)) {
        gfx_round_rect(x, y, SIDE_W, 76, 18, C_PANEL);
        gfx_button(x + 20, y + 23, "Y", ui_text("Edit game password"));
        add_zone(x, y, SIDE_W, 76, HidNpadButton_Y);
        y += 92;
    }
    if (v->status_valid && v->config_state == RelayConfigReady && !v->appointment_game_id && !v->private_panel && !v->private_room && !show_matching(v) &&
        v->st.state != StateInRoom && v->st.state != StateConnecting &&
        v->discovery == LobbyDiscoveryLegacy && !is_self_network(v) && !lobby_provider_is_lanplay(v->config.provider) &&
        !can_change_host_region(v)) {
        gfx_round_rect(x, y, SIDE_W, 80, 18, C_PANEL);
        gfx_button(x + 20, y + 25, "Y", TR("Sala de amigos", "Friends room"));
        add_zone(x, y, SIDE_W, 80, HidNpadButton_Y);
        y += 96;
    }
    if (v->status_valid && v->config_state == RelayConfigReady && !v->appointment_game_id && !v->private_panel && !v->private_room && !show_matching(v) &&
        v->st.state != StateInRoom && v->st.state != StateConnecting &&
        supports_reserved(v)) {
        gfx_round_rect(x, y, SIDE_W, 80, 18, C_PANEL);
        gfx_button(x + 20, y + 25, "L", TR("Buscar partida", "Quick match"));
        add_zone(x, y, SIDE_W, 80, HidNpadButton_L);
        y += 96;
    }
    if (v->discovery == LobbyDiscoveryUnified && v->unified.total && !v->appointment_game_id &&
        !show_matching(v) && !v->private_room && !v->private_panel) {
        char label[80];
        snprintf(label, sizeof(label), ui_text("%u of %u sources online"), v->unified.available, v->unified.total);
        gfx_text_fit(x + 20, y + 11, 19, v->unified.available < v->unified.total ? C_WARN : C_DIM,
                     label, SIDE_W - 40);
    }
}

static void draw_brand(void)
{
    const u32 link = RGBA(0xBE, 0xBE, 0xBE, 0xFF);
    gfx_rect(MARGIN + 16, 34, 2, 8, link);
    gfx_rect(MARGIN + 7, 40, 20, 2, link);
    gfx_rect(MARGIN + 7, 41, 2, 5, link);
    gfx_rect(MARGIN + 25, 41, 2, 5, link);
    gfx_round_rect(MARGIN + 9, 26, 16, 10, 3, C_TEXT);
    gfx_round_rect(MARGIN, 44, 16, 10, 3, C_TEXT);
    gfx_round_rect(MARGIN + 18, 44, 16, 10, 3, C_TEXT);
    gfx_rect(MARGIN + 13, 29, 8, 4, C_PANEL);
    gfx_rect(MARGIN + 4, 47, 8, 4, C_PANEL);
    gfx_rect(MARGIN + 22, 47, 8, 4, C_PANEL);
    gfx_text(MARGIN + 46, 25, 32, C_TEXT, "RoomCircuit");
    char version[32];
    snprintf(version, sizeof(version), "v%s", update_ui_app_version());
    gfx_text_fit(MARGIN + 46, 57, 17, C_DIM, version, 218);
}

static void draw_update_action(int x, int y, bool shortcut)
{
    const bool available = update_ui_available();
    const char *label = update_ui_button_label(available);
    const int key_width = shortcut ? gfx_text_width(16, "LS") + 12 : 0;
    const int key_gap = shortcut ? 8 : 0;
    const int width = 20 + key_width + key_gap + gfx_text_width(18, label);
    gfx_round_rect(x, y, width, 38, 12, available ? C_OK : C_CARD);
    if (shortcut) {
        gfx_round_rect(x + 10, y + 8, key_width, 22, 11, available ? C_PANEL : C_LINE);
        gfx_text(x + 16, y + 11, 16, available ? C_OK : C_DIM, "LS");
    }
    gfx_text(x + 10 + key_width + key_gap, y + 10, 18, available ? C_PANEL : C_DIM, label);
    add_zone(x - 8, y - 8, width + 16, 54, TOUCH_UPDATE);
}

static const char *board_game_title(const View *v, u64 game, char *buffer, size_t size)
{
    if (!game) return ui_text("All games");
    const CatalogMatch match = game_catalog_identify(&g_catalog, 0, game);
    if (match.kind != CatalogShared) {
        for (int i = 0; i < v->count && i < LOBBY_PAGE_SIZE; ++i)
            if (v->groups[i].game_id == game)
                return room_title(&v->groups[i], buffer, size);
    }
    const char *name = game_catalog_name(&match, g_zh);
    if (name) return name;
    snprintf(buffer, size, TR("ID del juego %016lX", "Game ID %016lX"), (unsigned long)game);
    return buffer;
}

static const char *board_room_node(const View *v, int index)
{
    if (!v->sources[index][0]) return ui_text("Main server");
    if (v->node_rows[index].node_id >= NODE_CATALOG_COUNT && nodes_ui_valid(v->node_rows[index].node_id))
        return nodes_ui_name(v->node_rows[index].node_id);
    if (strcmp(v->sources[index], REGION_PROVIDER_EU) == 0)
        return region_label(RelayRegionEurope);
    return provider_label(lobby_provider_index(v->sources[index]));
}

static const char *board_room_region(const View *v, int index)
{
    if (strcmp(v->sources[index], REGION_PROVIDER_EU) == 0)
        return ui_text("Europe");
    return nodes_ui_location(v->node_rows[index].node_id);
}

static void draw_room_board(const View *v)
{
    enum { X = 40, Y = 151, RIGHT = 1240, BOTTOM = 564, HEAD = 41, ROW = 62 };
    const bool filtered = v->game_filter != 0;
    const bool selected_room = v->sel >= 0 && v->sel < v->count && v->sel < LOBBY_PAGE_SIZE;
    const unsigned pane = filtered ? 1 : selected_room ? 2 : 0;
    const u64 title_id = filtered ? v->game_filter : selected_room ? v->groups[v->sel].game_id : 0;
    const u64 stamp = now_ms();
    if (title_id != g_board_title_id || pane != g_board_title_pane || g_zh != g_board_title_chinese) {
        g_board_title_id = title_id;
        g_board_title_pane = pane;
        g_board_title_chinese = g_zh;
        g_board_title_started = stamp;
    }
    const u64 title_elapsed = stamp - g_board_title_started;

    int page_right = RIGHT - 18;
    if (v->next_page != LOBBY_PAGE_END) {
        const char *next = ui_text("Next");
        const int width = gfx_button_width("ZR", next);
        const int x = page_right - width;
        gfx_button(x, 100, "ZR", next);
        add_zone(x - 8, 92, width + 16, 51, HidNpadButton_ZR);
        page_right = x - 24;
    }
    if (v->page_index > 0) {
        const char *prev = ui_text("Prev");
        const int width = gfx_button_width("ZL", prev);
        const int x = page_right - width;
        gfx_button(x, 100, "ZL", prev);
        add_zone(x - 8, 92, width + 16, 51, HidNpadButton_ZL);
        page_right = x - 24;
    }
    char filter_label[96];
    const char *filter_action = ui_text("Filter rooms");
    if (v->region_filter != 255) {
        snprintf(filter_label, sizeof(filter_label), "%s · %s", filter_action, nodes_ui_region(v->region_filter));
        filter_action = filter_label;
    }
    const int filter_width = gfx_button_width("ZL+ZR", filter_action);
    const int filter_x = page_right - filter_width;
    gfx_button(filter_x, 100, "ZL+ZR", filter_action);
    add_zone(filter_x - 8, 92, filter_width + 16, 51, TOUCH_NODE_FILTER);

    char buffer[160];
    const char *heading = filtered ? board_game_title(v, v->game_filter, buffer, sizeof(buffer)) : ui_text("Public rooms");
    const bool quick = v->status_valid && supports_reserved(v) &&
        v->st.state != StateInRoom && v->st.state != StateConnecting;
    const char *quick_label = TR("Buscar partida", "Quick match");
    const int quick_width = quick ? gfx_button_width("L", quick_label) + 24 : 0;
    const int heading_width = filter_x - X - quick_width - 24;
    const int heading_y = 96;
    if (filtered)
        g_board_title_animates |= gfx_text_marquee(X, heading_y, 31, C_TEXT, heading, heading_width, title_elapsed);
    else
        gfx_text_fit(X, heading_y, 31, C_TEXT, heading, heading_width);
    if (quick) {
        const int title_width = gfx_text_width(31, heading);
        const int match_x = X + (title_width < heading_width ? title_width : heading_width) + 24;
        const int width = gfx_button(match_x, 100, "L", quick_label);
        add_zone(match_x - 8, 92, width + 16, 51, HidNpadButton_L);
    }

    gfx_round_rect(X, Y, RIGHT - X, BOTTOM - Y, 12, C_LINE);
    gfx_round_rect(X + 1, Y + 1, RIGHT - X - 2, BOTTOM - Y - 2, 11, C_PANEL);
    const int title_x = X + 20, col_node = 704, col_access = 971, col_players = 1135;
    gfx_text(title_x, Y + 12, 18, C_DIM, ui_text("Game"));
    gfx_text(col_node, Y + 12, 18, C_DIM, ui_text("Node"));
    gfx_text(col_access, Y + 12, 18, C_DIM, ui_text("Password"));
    gfx_text(col_players, Y + 12, 18, C_DIM, ui_text("Players"));
    gfx_rect(X + 1, Y + HEAD - 1, RIGHT - X - 2, 1, C_LINE);
    int y = Y + HEAD;
    if (v->count <= 0) {
        const char *state = v->count == ListConnecting ? TR("Conectando…", "Connecting…") :
            v->count == ListUnavailable ? TR("Lista no disponible", "List unavailable") :
            v->count < 0 ? list_error_title(v->list_status) : ui_text("No open groups yet");
        gfx_text_fit(title_x, y + 27, 25, v->count < 0 ? C_WARN : C_TEXT, state, RIGHT - title_x - 18);
        if (v->count == ListOffline)
            gfx_text_fit(title_x, y + 68, 20, C_DIM, list_error_hint(v->list_status, NULL), RIGHT - title_x - 18);
        return;
    }
    for (int i = 0; i < v->count && i < LOBBY_PAGE_SIZE; ++i, y += ROW) {
        const LobbyGroup *g = &v->groups[i];
        const bool own = owns_direct_room(v, &v->node_rows[i]);
        const bool selected = v->sel == i;
        const bool full = lobby_room_full(g);
        if (selected) {
            gfx_rect(X + 1, y, RIGHT - X - 2, ROW, C_ACCENT);
            gfx_rect(X + 1, y, 3, ROW, C_OK);
        } else {
            gfx_rect(X + 1, y + ROW - 1, RIGHT - X - 2, 1, C_LINE);
        }

        const GameInfo *info = display_game_icon(g);
        const int game_x = title_x + (info && info->icon.px ? 46 : 0);
        if (info && info->icon.px) gfx_image(&info->icon, title_x, y + 13, 36, 36);
        const char *title = own ? direct_host_title(v) : NULL;
        if (!title) title = room_title(g, buffer, sizeof(buffer));
        if (selected)
            g_board_title_animates |= gfx_text_marquee(game_x, y + 19, 23, C_TEXT, title, col_node - game_x - 16, title_elapsed);
        else
            gfx_text_fit(game_x, y + 19, 23, C_TEXT, title, col_node - game_x - 16);

        char node[120];
        snprintf(node, sizeof(node), "%s · %s", board_room_node(v, i), board_room_region(v, i));
        gfx_text_fit(col_node, y + 21, 20, C_DIM, node, col_access - col_node - 14);
        const char *access = g->room_access == RoomAccessHidden ? ui_text("Required") :
            g->room_access == RoomAccessShared ? ui_text("Shared") :
            (v->node_rows[i].flags == NodeRoomDirect && v->node_rows[i].node_id >= 2) ? ui_text("In game") : NULL;
        if (access) gfx_text_fit(col_access, y + 21, 19, g->room_access == RoomAccessHidden ? C_WARN : C_DIM,
                                 access, col_players - col_access - 18);
        char players[20];
        snprintf(players, sizeof(players), "%u / %u", g->players, g->max_players);
        const int player_x = RIGHT - 18 - gfx_text_width(21, players);
        gfx_text(player_x, y + 21, 21, full ? C_WARN : C_TEXT, players);
        g_rendered_rooms[i] = lobby_room_id_on(g, v->sources[i]);
        g_rendered_node_rows[i] = v->node_rows[i];
        add_zone(X + 1, y, RIGHT - X - 2, ROW, TOUCH_CARD | (u64)i);
    }
}

static void draw_board_header(const View *v)
{
    gfx_rect(0, 0, GFX_W, 83, C_PANEL);
    gfx_rect(0, 82, GFX_W, 1, C_LINE);
    draw_brand();
    const char *appointments = ui_text("Appointments");
    const int width = gfx_button_width("-", appointments);
    const int x = GFX_W - MARGIN - width;
    gfx_button(x, 26, "-", appointments);
    add_zone(x - 8, 12, width + 16, 61, HidNpadButton_Minus);
    if (update_ui_available())
        draw_update_action(310, 22, !can_change_provider(v));
}

static void draw_password_visibility(void)
{
    draw_brand();
    gfx_round_rect(MARGIN, 160, LIST_W, 340, 18, C_PANEL);
    gfx_text(MARGIN + 28, 185, 28, C_TEXT, ui_text("Who can see the password?"));
    const int x = MARGIN + 28;
    gfx_button(x, 300, "A", ui_text("Share in lobby"));
    add_zone(x - 8, 284, LIST_W - 44, 62, HidNpadButton_A);
    gfx_button(x, 370, "X", ui_text("Keep password hidden"));
    add_zone(x - 8, 354, LIST_W - 44, 62, HidNpadButton_X);
    gfx_button(x, 440, "B", ui_text("Cancel"));
    add_zone(x - 8, 424, LIST_W - 44, 62, HidNpadButton_B);
}

static void draw_update_entry(bool shortcut)
{
    draw_update_action(350, 41, shortcut);
}

static void draw_appointments_link(const View *v)
{
    if (!appointment_entry_available(v)) return;
    const int width = gfx_button(SIDE_X, 92, "-", ui_text("Appointments"));
    add_zone(SIDE_X - 8, 85, width + 16, 43, HidNpadButton_Minus);
}

static void draw_board_route(const View *v, int right_actions)
{
    const bool direct = has_direct_host(v);
    const bool edit_password = can_share_game_password(v);
    const bool hosting = direct || edit_password;
    char label[160], route[256];
    const char *name = direct ? direct_host_title(v) : edit_password ?
        board_game_title(v, v->st.game_id, label, sizeof(label)) : hosting_node_label(v, label, sizeof(label));
    if (!name) name = nodes_ui_name(v->direct_host.node_id);
    const char *waiting = direct && !edit_password ? ui_text("Syncing room details…") : NULL;
    snprintf(route, sizeof(route), "%s · %s%s%s", ui_text(hosting ? "My room" : "Hosting line"),
             name, waiting ? " · " : "", waiting ? waiting : "");

    const char *action = edit_password ? ui_text("Edit password") :
        can_change_host_region(v) ? ui_text("Change") : NULL;
    const int action_width = action ? gfx_button_width("Y", action) : 0;
    const int room = right_actions - MARGIN - 24 - (action ? action_width + 16 : 0);
    if (room <= 0) return;
    gfx_text_fit(MARGIN, FOOTER_Y + 26, 19, C_OK, route, room);
    if (action) {
        const int route_width = gfx_text_width(19, route);
        const int action_x = MARGIN + (route_width < room ? route_width : room) + 16;
        gfx_button(action_x, FOOTER_Y + 20, "Y", action);
        add_zone(MARGIN - 8, FOOTER_Y + 8, action_x + action_width - MARGIN + 16, 56, HidNpadButton_Y);
    }
}

static void draw(const View *v)
{
    g_board_title_animates = false;
    gfx_begin();
    gfx_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    g_zone_count = 0;
    memset(g_rendered_rooms, 0, sizeof(g_rendered_rooms));
    memset(g_rendered_node_rows,0,sizeof(g_rendered_node_rows));

    if (g_guide_open) {
        guide_draw(g_guide_page, g_zh, g_es);
        save_capture_if_requested();
        gfx_end();
        return;
    }
    if (appointments_active()) {
        appointments_draw(&g_catalog, add_zone);
        if (update_ui_available()) draw_update_action(310, 22, true);
        save_capture_if_requested();
        gfx_end();
        return;
    }
    if (v->password_edit.active) {
        draw_password_visibility();
        save_capture_if_requested();
        gfx_end();
        return;
    }
    const bool board = room_board_available(v);
    if (board) {
        draw_board_header(v);
        draw_room_board(v);
    } else {
        draw_brand();
        status_pill(v);
        draw_appointments_link(v);
        if (v->provider_panel) provider_column(v);
        else if (v->private_panel || v->private_room) private_column(v);
        else groups_column(v);
        if (!v->provider_panel && !has_pinned_direct_room(v)) side_column(v);
        draw_update_entry(!can_change_provider(v));
    }
    if (v->room_notice_visible)
        gfx_text_fit(MARGIN, FOOTER_Y - 36, 21, v->st.state == StateConnecting ? C_WARN : C_ERR,
                     tr_error(v->st.error), GFX_W - 2 * MARGIN);
    else if (v->storage_health)
        gfx_text_fit(MARGIN, FOOTER_Y - 36, 21, C_WARN, storage_health_text(v->storage_health), GFX_W - 2 * MARGIN);
    else if (v->inventory_error)
        gfx_text_fit(MARGIN, FOOTER_Y - 36, 21, C_ERR,
                     TR("No se pudieron leer los nombres. Vuelve a abrir la app.", "Could not read game titles. Reopen the app."), GFX_W - 2 * MARGIN);
    else if (v->message[0])
        gfx_text_fit(MARGIN, FOOTER_Y - 36, 21, C_CODE, v->message, GFX_W - 2 * MARGIN);

    gfx_rect(0, FOOTER_Y, GFX_W, GFX_H - FOOTER_Y, C_CARD);
    gfx_rect(0, FOOTER_Y, GFX_W, 1, C_LINE);
    const char *buttons[8], *labels[8];
    int n = 0;
    if (v->provider_panel) {
        buttons[n] = "A"; labels[n++] = TR("Elegir", "Select");
        buttons[n] = "B"; labels[n++] = TR("Volver", "Back");
    } else if (v->external_confirm) {
        if (!external_room_expired(v) && local_game_info(&v->pinned_group)) {
            buttons[n] = "A"; labels[n++] = TR("Abrir juego", "Open game");
        }
        buttons[n] = "B"; labels[n++] = TR("Cancelar", "Cancel selection");
    } else if (show_matching(v)) {
        if (can_enter_game_password(v)) {
            buttons[n] = "A"; labels[n++] = ui_text("Enter game password");
        } else if (supports_reserved(v) && v->match.mode == LobbyQuick && (v->match.state == MatchExpired || v->match.state == MatchError)) {
            buttons[n] = "A"; labels[n++] = TR("Reintentar", "Try again");
        } else if (supports_reserved(v) && v->match.mode == LobbyExact && v->pinned.game_id && room_password_allows_launch(v) &&
                   v->match.state >= MatchWaitScan && v->match.state <= MatchAssigned) {
            if (local_game_info(&v->pinned_group)) { buttons[n] = "A"; labels[n++] = TR("Abrir juego", "Open game"); }
        }
        buttons[n] = "B"; labels[n++] = TR("Cancelar", "Cancel selection");
    } else if (v->private_room) {
        buttons[n] = "B"; labels[n++] = TR("Salir de la sala", "Leave room");
    } else if (v->private_panel) {
        buttons[n] = "B"; labels[n++] = TR("Volver", "Back");
    } else if (v->sel >= 0 && v->sel < v->count && v->sel < LOBBY_PAGE_SIZE &&
               !lobby_room_full(&v->groups[v->sel]) && !owns_direct_room(v, &v->node_rows[v->sel])) {
        buttons[n] = "A"; labels[n++] = TR("Elegir sala", "Select room");
    } else if (v->appointment_game_id) {
        buttons[n] = "B"; labels[n++] = ui_text("Appointments");
    }
    if (!board && !v->provider_panel && can_change_provider(v)) {
        const char *network = provider_label(lobby_provider_index(v->config.provider));
        int nw = gfx_button(MARGIN, FOOTER_Y + 20, "LS", network);
        add_zone(MARGIN - 10, FOOTER_Y + 8, nw + 20, 56, HidNpadButton_StickL);
    }
    int total = 0;
    for (int i = 0; i < n; i++)
        total += gfx_button_width(buttons[i], labels[i]) + (i ? 28 : 0);
    int x = GFX_W - MARGIN - total;
    if (board) draw_board_route(v, x);
    for (int i = 0; i < n; i++) {
        if (board && buttons[i][0] == 'A')
            gfx_round_rect(x - 8, FOOTER_Y + 12, 56 + gfx_text_width(22, labels[i]), 48, 8, C_ACCENT);
        int w = gfx_button(x, FOOTER_Y + 20, buttons[i], labels[i]);
        add_zone(x - 10, FOOTER_Y + 8, w + 20, 56, button_mask(buttons[i]));  // algo más grande que el dibujo, para el dedo
        x += w + 28;
    }

    if (!v->password.value[0]) save_capture_if_requested();
    gfx_end();
}

static void draw_busy(const char *text)
{
    gfx_begin();
    gfx_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    int w = gfx_text_width(28, text);
    gfx_text((GFX_W - w) / 2, GFX_H / 2 - 20, 28, C_TEXT, text);
    gfx_end();
}

static const char *g_keyboard_error;

static bool ask_code(char *out, size_t size)
{
    g_keyboard_error = NULL;
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        g_keyboard_error = "Could not open the keyboard. Try again.";
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, TR("Código de la sala", "Room code"));
    swkbdConfigSetGuideText(&kbd, TR("6 letras o números", "6 letters or numbers"));
    swkbdConfigSetStringLenMax(&kbd, 6);
    swkbdConfigSetStringLenMin(&kbd, 6);
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    if (R_FAILED(rc) && rc != MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit))
        g_keyboard_error = "Could not open the keyboard. Try again.";
    for (char *p = out; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
    return R_SUCCEEDED(rc) && strlen(out) == 6;
}

#if !RELAY_PROFILE_FIXED
static bool ask_server_field(bool address, const char *current, char *out, size_t size)
{
    g_keyboard_error = NULL;
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        g_keyboard_error = "Could not open the keyboard. Try again.";
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    if (address)
        swkbdConfigSetLeftOptionalSymbolKey(&kbd, ".");
    swkbdConfigSetHeaderText(&kbd, address ? TR("Dirección IPv4 del servidor", "Relay server IPv4 address") :
                                           TR("Puerto del servidor", "Relay server port"));
    swkbdConfigSetGuideText(&kbd, address ? "203.0.113.10" : "11460");
    swkbdConfigSetInitialText(&kbd, current);
    swkbdConfigSetStringLenMin(&kbd, address ? 7 : 1);
    swkbdConfigSetStringLenMax(&kbd, address ? RELAY_IPV4_SIZE - 1 : 5);
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    if (R_FAILED(rc) && rc != MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit))
        g_keyboard_error = "Could not open the keyboard. Try again.";
    return R_SUCCEEDED(rc);
}
#endif

static bool ensure_config_dir(void)
{
    return mkdir(RELAY_CONFIG_DIR, 0777) == 0 || errno == EEXIST;
}

static void config_write_error(View *v)
{
    snprintf(v->message, sizeof(v->message), "%s", TR("No se pudo guardar en la tarjeta SD.", "Could not save to the SD card."));
}

#if !RELAY_PROFILE_FIXED
static bool edit_server(View *v)
{
    if (show_matching(v)) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Cancela la búsqueda antes de cambiar de servidor.", "Cancel matching before changing server."));
        return false;
    }
    if (v->st.state == StateInRoom || v->st.state == StateConnecting) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Sal de la sala antes de cambiar el servidor.", "Leave your room before changing server."));
        return false;
    }

    char address[RELAY_IPV4_SIZE] = {0};
    if (!ask_server_field(true, v->config.server, address, sizeof(address))) {
        if (g_keyboard_error) snprintf(v->message, sizeof(v->message), "%s", ui_text(g_keyboard_error));
        return false;
    }
    if (!relay_config_ipv4_valid(address)) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Introduce una IPv4 válida.", "Enter a valid IPv4 address."));
        return false;
    }

    char current_port[6], port_text[6] = {0};
    snprintf(current_port, sizeof(current_port), "%u", (unsigned)v->config.port);
    if (!ask_server_field(false, current_port, port_text, sizeof(port_text))) {
        if (g_keyboard_error) snprintf(v->message, sizeof(v->message), "%s", ui_text(g_keyboard_error));
        return false;
    }
    uint16_t port;
    if (!relay_config_parse_port(port_text, &port)) {
        snprintf(v->message, sizeof(v->message), "%s", TR("El puerto debe ser de 1 a 65535.", "Port must be 1 to 65535."));
        return false;
    }

    if (!ensure_config_dir() || !relay_config_save_endpoint(RELAY_CONFIG_PATH, address, port)) {
        config_write_error(v);
        return false;
    }
    memcpy(v->config.server, address, strlen(address) + 1);
    v->config.port = port;
    v->config_state = RelayConfigReady;
    snprintf(v->message, sizeof(v->message), "%s", TR("Servidor guardado.", "Relay saved."));
    return true;
}
#endif


static void read_direct_host(View *v)
{
    v->direct_host = (DirectRoomHost){0};
    if (!v->status_valid || v->st.state != StateInRoom || v->st.my_index != 0 ||
        !(v->host_regions & RELAY_REGION_COMMUNITY_ACTIVE)) return;
    DirectRoomHost host = {0};
    if (R_FAILED(serviceDispatch(&g_lanywhere, DIRECT_ROOM_HOST_IPC,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { &host, sizeof(host) } })) ||
        !host.epoch || host.node_id < 2 || !nodes_ui_valid(host.node_id) || !host.host_ip || host.reserved_ip ||
        !(host.flags & DirectRoomHostActive) || (host.flags & ~(DirectRoomHostActive | DirectRoomHostCanEdit)) ||
        host.reserved[0] || host.reserved[1] || !memcmp(host.bssid, (uint8_t[6]){0}, 6) ||
        !memcmp(host.network_id + 16, (uint8_t[16]){0}, 16) ||
        memcmp(host.network_id, &v->st.game_id, sizeof(v->st.game_id))) return;
    LobbyGroup title = { .metadata_flags = LOBBY_META_LOCAL };
    memcpy(title.title_en, host.title_en, sizeof(title.title_en));
    memcpy(title.title_zh, host.title_zh, sizeof(title.title_zh));
    lobby_sanitize_group(&title);
    memcpy(host.title_en, title.title_en, sizeof(host.title_en));
    memcpy(host.title_zh, title.title_zh, sizeof(host.title_zh));
    v->direct_host = host;
}

static void poll_lanywhere(View *v)
{
    if (v->node_picker_available) {
        NodeHostInfo info = {0};
        Result rc = serviceDispatch(&g_lanywhere, NODE_GET_HOST_INFO_IPC,
            .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
            .buffers = { { &info, sizeof(info) } });
        v->host_info_valid = R_SUCCEEDED(rc) && info.version == 1 && info.status <= NodeHostSelected &&
            !info.reserved && info.name[0] && memchr(info.name, '\0', sizeof(info.name));
        if (v->host_info_valid) { v->host_info = info; v->host_node = info.node_id; }
    }
    LanywhereStatus status = {0};
    if (R_SUCCEEDED(lanywhere_status(&status)) && status.state <= StateError) {
        status.code[sizeof(status.code) - 1] = '\0';
        status.error[sizeof(status.error) - 1] = '\0';
        if (status.state != v->st.state || strcmp(status.error, v->st.error) != 0) {
            v->room_notice_until = status.error[0] ? now_ms() + ROOM_NOTICE_MS : 0;
            v->room_notice_from_session = v->st.state == StateConnecting || v->st.state == StateInRoom;
        }
        v->st = status;
        v->status_valid = true;
        v->private_room = (status.state == StateInRoom || (status.state == StateConnecting && status.code[0])) &&
                          status.game_id == 0;
    } else {
        v->status_valid = false;
    }
    if (v->host_region_available && v->status_valid)
        v->host_regions = lanywhere_host_regions(v->capabilities);
    read_direct_host(v);
    LobbyMatch match = {0};
    v->match_available = R_SUCCEEDED(lanywhere_match_status(&match)) && match.mode <= LobbyExact && match.state <= MatchError;
    if (v->match_available) {
        match.room[sizeof(match.room) - 1] = '\0';
        match.error[sizeof(match.error) - 1] = '\0';
        v->match = match;
    }
}

static bool show_room_notice(const View *v, u64 now)
{
    if (!v->status_valid || !v->st.error[0]) return false;
    if (v->st.state == StateConnecting || v->st.state == StateInRoom) return true;
    return now < v->room_notice_until &&
           (v->room_notice_from_session || !lobby_transport_error_is_stale(v->st.error, v->count));
}

static void cancel_match(View *v)
{
    draw_busy(TR("Cancelando la búsqueda…", "Cancelling matching…"));
    if (R_FAILED(lanywhere_cancel_match())) {
        snprintf(v->message, sizeof(v->message), "%s", TR("No se pudo cancelar. Inténtalo de nuevo.", "Could not cancel. Try again."));
        return;
    }
    v->pinned = (LobbyRoomId){0};
    memset(&v->pinned_group, 0, sizeof(v->pinned_group));
    v->selection = (LobbyRoomId){0};
    v->sel = -1;
    snprintf(v->message, sizeof(v->message), "%s", TR("Selección cancelada.", "Selection cancelled."));
    poll_lanywhere(v);
}

static void start_quick(View *v)
{
    if (!v->status_valid) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Estado del complemento no disponible.", "Plugin status unavailable."));
    } else if (!supports_reserved(v)) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Esta red no admite búsqueda reservada.", "This network does not support reserved matching."));
    } else if (!RELAY_PROFILE_FIXED && v->config_state != RelayConfigReady) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Configura el servidor antes de buscar.", "Set up your relay before matching."));
    } else if (v->st.state == StateInRoom || v->st.state == StateConnecting) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Sal de la sala antes de buscar.", "Leave your room before matching."));
    } else {
        if (show_matching(v))
            draw_busy(TR("Reiniciando la búsqueda…", "Restarting Quick match…"));
        if (R_FAILED(lanywhere_start_quick())) {
            snprintf(v->message, sizeof(v->message), "%s", TR("No se pudo activar la búsqueda rápida.", "Could not arm Quick match."));
            poll_lanywhere(v);
            return;
        }
        v->selection = (LobbyRoomId){0};
        v->pinned = (LobbyRoomId){0};
        memset(&v->pinned_group, 0, sizeof(v->pinned_group));
        v->sel = -1;
        v->message[0] = '\0';
        poll_lanywhere(v);
    }
}

static const char *external_join_hint(const char *provider, bool installed)
{
    if (strcmp(provider, REGION_PROVIDER_COMMUNITY) == 0 || lobby_provider_is_lanplay(provider))
        return installed ? ui_text("Press A to open the game, then search in Local Wireless.") :
            ui_text("Open the game from HOME, then search in Local Wireless.");
    return installed ? TR("Sala no reservada. Pulsa A para abrir el juego y buscar.", "Room not reserved. Press A to open the game and search.") :
        TR("Abre el juego desde HOME y busca la sala; no está reservada.", "Open the game from HOME and search; this room is not reserved.");
}

static void select_room(View *v)
{
    LobbyRoomId selection = v->selection;
    LobbyNodeSelection selected_node = {0};
    const bool legacy_external = !selection.provider[0] &&
        v->discovery == LobbyDiscoveryLegacy && !is_self_network(v);
    if (legacy_external)
        snprintf(selection.provider, sizeof(selection.provider), "%s", v->config.provider);
    const bool routed = selection.provider[0] != '\0';
    const bool community = strcmp(selection.provider, REGION_PROVIDER_COMMUNITY) == 0;
    const bool reserved = routed ? lobby_provider_is_self_relay(selection.provider) : supports_reserved(v);
    if (routed || reserved) {
        if (community) draw_busy(ui_text("Checking community connection…"));
        Result result;
        const int node_index=v->node_directory && !v->appointment_game_id ?
            lobby_room_index_on(selection,v->groups,v->sources,(size_t)(v->count>0?v->count:0)):-1;
        if(node_index>=0 && v->node_rows[node_index].flags==NodeRoomDirect){
            const LobbyNodeRoom *room=&v->node_rows[node_index];
            const LobbyNodeSelection request={.game_id=room->group.game_id,.node_id=room->node_id,.locator=room->locator};
            result=lanywhere_select_node(request);
            if(R_SUCCEEDED(result))selected_node=request;
        } else result = routed ? lanywhere_select_unified(selection) : lanywhere_select(selection.code);
        if (routed && unified_ipc_missing(result)) {
            if (legacy_external) {
                selection = v->selection;
                result = 0;
            } else if (v->appointment_game_id && is_self_network(v) && strcmp(selection.provider, "self") == 0) {
                result = lanywhere_select(selection.code);
            }
        }
        if (R_FAILED(result)) {
            snprintf(v->message, sizeof(v->message), "%s",
                result == MAKERESULT(450, RELAY_COMMUNITY_UNREACHABLE_DESCRIPTION) ?
                ui_text("Could not connect to this community room.") :
                TR("No se pudo seleccionar la sala.", "Could not select that room."));
            poll_lanywhere(v);
            return;
        }
    }
    v->pinned = selection;
    v->pinned_node = selected_node;
    if (v->sel >= 0 && v->sel < v->count) v->pinned_group = v->groups[v->sel];
    v->selection = (LobbyRoomId){0};
    v->sel = -1;
    v->message[0] = '\0';
    if (!reserved) {
        v->external_confirm = true;
        const char *hint = external_join_hint(selection.provider, local_game_info(&v->pinned_group) != NULL);
        snprintf(v->message, sizeof(v->message), "%s", hint);
    }
    if (selection.provider[0] || reserved) poll_lanywhere(v);
}

static bool open_selected_game(View *v)
{
    if (!room_password_allows_launch(v)) return false;
    const GameInfo *info = v->pinned.game_id ? local_game_info(&v->pinned_group) : NULL;
    if (!info)
        return false;
    char busy[200];
    snprintf(busy, sizeof(busy), TR("Abriendo %s…", "Opening %s…"), info->name);
    draw_busy(busy);
    if (R_FAILED(appletRequestLaunchApplication(info->id, NULL))) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Abre el juego desde HOME y busca el grupo.", "Open the game from HOME and search for groups."));
        return false;
    }
    return true;
}

static void draw_no_service(bool incompatible)
{
    g_guide_open = false;
    gfx_begin();
    gfx_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    draw_brand();
    gfx_round_rect(MARGIN, 140, GFX_W - 2 * MARGIN, 200, 18, C_PANEL);
    gfx_text(MARGIN + 28, 170, 28, C_TEXT, !roomcircuit_storage_available() ?
        ui_text("Restart to finish setup or check the SD card.") : incompatible ?
        TR("La app y el módulo son de versiones distintas", "The app and sysmodule versions do not match") :
        TR("Falta el componente de RoomCircuit", "The RoomCircuit component is missing"));
    gfx_text(MARGIN + 28, 222, 22, C_DIM, ui_text("Open Update to repair the installation."));
    gfx_text(MARGIN + 28, 254, 22, C_DIM, TR("y reinicia la consola.", "and restart the console."));
    gfx_rect(0, FOOTER_Y, GFX_W, 1, C_LINE);
    g_zone_count = 0;
    draw_update_entry(true);
    gfx_end();
}

static void no_service(PadState *pad, bool incompatible)
{
    draw_no_service(incompatible);
    while (appletMainLoop()) {
        padUpdate(pad);
        const u64 down = padGetButtonsDown(pad) | touch_button();
        if (down & HidNpadButton_Plus) break;
        if (down & (HidNpadButton_StickL | TOUCH_UPDATE)) {
            if (update_ui_open(pad, NULL, NULL)) break;
            draw_no_service(incompatible);
        }
        if (update_ui_changed()) draw_no_service(incompatible);
        svcSleepThread(33000000ULL);
    }
}

static bool update_install_ready(void *unused)
{
    (void)unused;
    LanywhereStatus status = {0};
    if (R_FAILED(lanywhere_status(&status)) || status.state > StateError ||
        status.state == StateConnecting || status.state == StateInRoom) return false;
    LobbyMatch match = {0};
    if (R_SUCCEEDED(lanywhere_match_status(&match)) && match.mode != LobbyManual &&
        match.state >= MatchWaitScan && match.state <= MatchJoining) return false;
    return true;
}

static void select_module_storage(void)
{
    /* Do not block waiting for a missing module or infer its data root from
     * a second filesystem read. Only the running module chooses the root. */
    u8 present = 0;
    const SmServiceName name = smEncodeName("lanywhr");
    if (R_FAILED(tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, present)) || !(present & 1)) return;
    Service service;
    if (R_FAILED(smGetService(&service, "lanywhr"))) return;
    u32 selection = 2;
    Result rc = serviceDispatchOut(&service, 46, selection);
    serviceClose(&service);
    if (R_SUCCEEDED(rc) && (selection == 0 || selection == 1 || selection == 3))
        roomcircuit_storage_set_generation(selection);
}

int main(int argc, char **argv)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    roomcircuit_storage_init();
    select_module_storage();
    detect_language();
    (void)game_catalog_open(&g_catalog, game_catalog_bin, game_catalog_bin_size);
    if (!gfx_init())
        return 1;
    if (update_ui_initialize(argc > 0 ? argv[0] : NULL)) {
        (void)update_ui_open(&pad, NULL, NULL);
        update_ui_shutdown();
        gfx_exit();
        return 0;
    }

    if (!roomcircuit_storage_available()) {
        no_service(&pad, true);
        update_ui_shutdown();
        gfx_exit();
        return 0;
    }

    if (R_FAILED(smGetService(&g_lanywhere, "lanywhr"))) {
        no_service(&pad, false);
        update_ui_shutdown();
        gfx_exit();
        return 0;
    }
    if (!lanywhere_protocol_supported()) {
        no_service(&pad, true);
        serviceClose(&g_lanywhere);
        update_ui_shutdown();
        gfx_exit();
        return 0;
    }
    nodes_ui_initialize();
    g_ns_ready = init_ns_with_diagnostic();
    Thread inventory;
    bool inventory_created = false, inventory_started = false;
    if (g_ns_ready) {
        atomic_store(&g_inventory_running, true);
        inventory_created = R_SUCCEEDED(threadCreate(&inventory, inventory_thread, NULL, NULL, 0x8000, 0x2C, -2));
        inventory_started = inventory_created && R_SUCCEEDED(threadStart(&inventory));
        if (!inventory_started) atomic_store(&g_inventory_running, false);
    }
    if (!g_ns_ready || !inventory_started) atomic_store(&g_inventory_finished, true);

    View v = {0};
    v.storage_health = read_storage_health(&g_lanywhere);
    v.sel = -1;
    v.region_filter=255;
    v.node_picker_available=nodes_ui_supported(&g_lanywhere,&v.host_node);
    v.node_directory = v.node_picker_available;
    g_nodes_directory=v.node_picker_available;
    if(!v.node_picker_available)snprintf(v.message,sizeof(v.message),"%s",ui_text("Restart with the matching background module to choose nodes."));
    v.next_page = LOBBY_PAGE_END;
    v.config_state = relay_config_load(RELAY_CONFIG_PATH, &v.config);
    v.host_region_available = lanywhere_host_region(&v.host_region);
    snprintf(g_list_provider, sizeof(g_list_provider), "%s", v.config.provider[0] ? v.config.provider : "self");
    mutexInit(&g_list_lock);
    ueventCreate(&g_refresh, true);
    appointments_init(&g_refresh);
    room_password_init(&g_refresh);
    g_list_enabled = v.config_state == RelayConfigReady;
    g_list_page = (LobbyPage){ .count = g_list_enabled ? ListConnecting : ListOffline, .next = LOBBY_PAGE_END };
    v.count = g_list_page.count;
    Thread thread;
    bool thread_created = R_SUCCEEDED(smGetService(&g_lanywhere_list, "lanywhr")) &&
                          R_SUCCEEDED(threadCreate(&thread, list_thread, NULL, NULL, 0x8000, 0x2C, -2));
    bool thread_ok = thread_created && R_SUCCEEDED(threadStart(&thread));
    if (!thread_ok)
        v.count = g_list_page.count = ListUnavailable;

    draw(&v);
    View last = {0};
    u32 page_offsets[MAX_PAGE_HISTORY] = {0};
    u64 last_draw = 0, last_capture_check = 0;
    u64 next_status = 0;
    u64 last_loop = now_ms();
    bool had_focus = appletGetFocusState() == AppletFocusState_InFocus;
    bool dirty = true;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 touch = touch_button();
        int card = -1;  // tarjeta de grupo tocada
        if (touch & TOUCH_CARD) {
            card = (int)(touch & 0xFF);
            touch = 0;
        }
        u64 down = padGetButtonsDown(&pad) | touch;
        const bool update_shortcut = (down & HidNpadButton_StickL) && !g_guide_open &&
            !v.password_edit.active && (appointments_active() || !can_change_provider(&v));
        if ((down & TOUCH_UPDATE) || update_shortcut) {
            if (update_ui_open(&pad, update_install_ready, NULL)) break;
            down = 0;
            dirty = true;
            next_status = 0;
            ueventSignal(&g_refresh);
        }
        if (update_ui_changed()) dirty = true;
        if (down & HidNpadButton_Plus) {
            if (atomic_load(&g_inventory_running))
                draw_busy(TR("Leyendo nombres de juegos…", "Reading game titles…"));
            break;
        }
        u64 now = now_ms();
        const bool has_focus = appletGetFocusState() == AppletFocusState_InFocus;
        if (has_focus && (!had_focus || now - last_loop > 5000)) {
            ueventSignal(&g_refresh);
            next_status = 0;
            clear_game_cache();
            nodes_ui_network_changed();
            v.password.refresh_at = 0;
        }
        last_loop = now;
        had_focus = has_focus;
        if (consume_inventory_result(&v)) {
            restart_group_list(page_offsets[v.page_index], v.page_index ? v.unified.snapshot : 0, thread_ok);
            dirty = true;
        }
        if (now >= next_status) {
            const NodeHostInfo previous_host = v.host_info;
            const bool previous_valid = v.host_info_valid;
            poll_lanywhere(&v);
            if (previous_valid != v.host_info_valid || memcmp(&previous_host, &v.host_info, sizeof(previous_host))) dirty = true;
            next_status = now + STATUS_POLL_MS;
        }
        if (v.external_confirm && (v.st.state == StateInRoom || v.st.state == StateConnecting)) {
            v.external_confirm = false;
            v.pinned = (LobbyRoomId){0};
            memset(&v.pinned_group, 0, sizeof(v.pinned_group));
            v.message[0] = '\0';
        }
        if (external_room_expired(&v))
            snprintf(v.message, sizeof(v.message), "%s", ui_text("Room selection expired. Press B and select again."));
        mutexLock(&g_list_lock);
        v.count = g_list_page.count;
        v.next_page = g_list_page.next;
        v.response_ms = g_list_response_ms;
        v.capabilities = g_list_page.capabilities;
        v.list_status = g_list_page.reserved;
        v.discovery = g_list_discovery;
        v.unified = g_list_unified;
        memcpy(v.groups, g_groups, sizeof(v.groups));
        memcpy(v.sources, g_list_sources, sizeof(v.sources));
        memcpy(v.node_rows,g_node_rows,sizeof(v.node_rows));
        v.node_directory=g_nodes_directory;
        if (g_list_changed)
            dirty = true;
        g_list_changed = false;
        mutexUnlock(&g_list_lock);
        if (v.list_status == UnifiedListStale && v.page_index) {
            v.page_index = 0;
            v.next_page = LOBBY_PAGE_END;
            v.selection = (LobbyRoomId){0};
            v.sel = -1;
            v.count = ListConnecting;
            restart_group_list(0, 0, thread_ok);
            dirty = true;
        }
        v.room_notice_visible = show_room_notice(&v, now);
        if (v.pinned.code[0] && v.count > 0) {
            const int pinned_index = lobby_room_index_on(v.pinned, v.groups, v.sources, (size_t)v.count);
            if (pinned_index >= 0) {
                const LobbyNodeRoom *row = &v.node_rows[pinned_index];
                if (!has_pinned_direct_room(&v) || (row->flags == NodeRoomDirect && row->node_id == v.pinned_node.node_id &&
                    !memcmp(&row->locator, &v.pinned_node.locator, sizeof(row->locator))))
                    v.pinned_group = v.groups[pinned_index];
                else
                    v.pinned_group.room_access = RoomAccessNone;
            }
        }
        if (thread_ok && update_room_password(&v, now)) dirty = true;
        if (v.password_edit.active) {
            const bool room_changed = !can_share_game_password(&v) ||
                memcmp(v.password_edit.change.room, v.st.code, sizeof(v.st.code)) != 0;
            if (room_changed || (down & HidNpadButton_B)) {
                v.password_edit = (RoomPasswordEdit){0};
                if (room_changed) snprintf(v.message, sizeof(v.message), "%s", ui_text("This game room is no longer available."));
            } else if (down & (HidNpadButton_A | HidNpadButton_X)) {
                const char *message = room_password_apply(&g_lanywhere, &v.password_edit, (down & HidNpadButton_X) != 0);
                if (message) snprintf(v.message, sizeof(v.message), "%s", message);
            }
            if (dirty || down || room_changed || now - last_draw >= 5000) {
                draw(&v);
                last_draw = now;
                dirty = false;
            }
            svcSleepThread(33000000ULL);
            continue;
        }
        if (now / 1000 != last_capture_check) {
            last_capture_check = now / 1000;
            if (capture_requested()) {
                g_capture_pending = true;
                dirty = true;
            }
        }
        if (appointments_tick(now)) dirty = true;
        if (guide_input(&v, down)) {
            if (down || dirty || now - last_draw >= 5000) {
                draw(&v);
                last_draw = now;
                dirty = false;
            }
            svcSleepThread(33000000ULL);
            continue;
        }
        if (appointments_active()) {
            u64 game_id = 0;
            const u64 held = padGetButtons(&pad);
            AppointmentAction action = appointments_input(down, held, now,
                atomic_load(&g_inventory_running), v.inventory_error, &game_id);
            if (action == AppointmentBack) {
                appointments_close();
                dirty = true;
            } else if (action == AppointmentFindRooms) {
                appointments_close();
                open_appointment_rooms(&v, game_id, thread_ok);
                dirty = true;
            }
            if (dirty || down || held || now - last_draw >= 1000 ||
                (appointments_title_animates() && now - last_draw >= 66)) {
                draw(&v);
                last_draw = now;
                dirty = false;
            }
            svcSleepThread(33000000ULL);
            continue;
        }
        if (room_filters_requested(&v, down, padGetButtons(&pad))) {
            if(nodes_ui_filters(&pad,&g_catalog,&v.game_filter,&v.region_filter)){
                mutexLock(&g_list_lock);g_node_game_filter=v.game_filter;g_node_region=v.region_filter;g_node_mask=nodes_ui_region_mask(v.region_filter);mutexUnlock(&g_list_lock);
                v.page_index=0;v.sel=-1;v.selection=(LobbyRoomId){0};
                v.count = thread_ok ? ListConnecting : ListUnavailable;
                v.next_page = LOBBY_PAGE_END;
                restart_group_list(0,0,thread_ok);
            }
            g_zh=ui_chinese();dirty=true;down=0;
        }
        if (((down & HidNpadButton_Minus) || (!room_board_available(&v) && (down & HidNpadButton_Left))) &&
            appointment_entry_available(&v)) {
            appointments_open(thread_ok, false);
            draw(&v);
            last_draw = now;
            dirty = false;
            continue;
        }
        if ((down & HidNpadButton_B) && v.appointment_game_id &&
            v.sel < 0 && !show_matching(&v) && !v.external_confirm) {
            v.appointment_game_id = 0;
            v.page_index = 0;
            v.next_page = LOBBY_PAGE_END;
            v.count = thread_ok ? ListConnecting : ListUnavailable;
            restart_group_query(0, 0, thread_ok, NULL, true, 0);
            appointments_open(thread_ok, true);
            draw(&v);
            last_draw = now;
            dirty = false;
            continue;
        }
        if (v.provider_panel) {
            if (down & TOUCH_NETWORK_CHOICE) {
                const int selected = (int)(down & 0xff);
                down &= ~(TOUCH_NETWORK_CHOICE | 0xffULL);
                if (selected < LOBBY_PROVIDER_COUNT) { v.provider_choice = selected; down |= HidNpadButton_A; }
            }
            if ((down & HidNpadButton_AnyLeft) && (v.provider_choice % 2)) --v.provider_choice;
            if ((down & HidNpadButton_AnyRight) && !(v.provider_choice % 2) && v.provider_choice + 1 < LOBBY_PROVIDER_COUNT) ++v.provider_choice;
            if ((down & HidNpadButton_AnyUp) && v.provider_choice >= 2) v.provider_choice -= 2;
            if ((down & HidNpadButton_AnyDown) && v.provider_choice + 2 < LOBBY_PROVIDER_COUNT) v.provider_choice += 2;
            else if ((down & HidNpadButton_AnyDown) && v.provider_choice == LOBBY_PROVIDER_COUNT - 2) ++v.provider_choice;
            if (down & HidNpadButton_B) {
                v.provider_panel = false;
            } else if ((down & HidNpadButton_A) && v.status_valid && !show_matching(&v) &&
                       (v.st.state == StateIdle || v.st.state == StateError)) {
                const char *provider = lobby_provider_id(v.provider_choice);
                if (!provider) { v.provider_choice = 0; provider = lobby_provider_id(0); }
                if (strcmp(v.config.provider, provider) != 0 &&
                    (!ensure_config_dir() || !relay_config_save_provider(RELAY_CONFIG_PATH, provider))) {
                    config_write_error(&v);
                } else {
                    snprintf(v.config.provider, sizeof(v.config.provider), "%s", provider);
                    v.provider_panel = false;
                    if (is_self_network(&v) || lobby_provider_is_lanplay(provider)) v.private_panel = false;
                    v.page_index = 0;
                    page_offsets[0] = 0;
                    v.selection = v.pinned = (LobbyRoomId){0};
                    memset(&v.pinned_group, 0, sizeof(v.pinned_group));
                    v.sel = -1;
                    v.capabilities = 0;
                    restart_group_list_on_network(0, thread_ok, provider);
                    v.count = thread_ok ? ListConnecting : ListUnavailable;
                    v.next_page = LOBBY_PAGE_END;
                    v.message[0] = '\0';
                    v.room_notice_visible = v.room_notice_from_session = false;
                    v.room_notice_until = 0;
                }
            }
            draw(&v);
            last_draw = now;
            dirty = false;
            continue;
        }
        if ((down & HidNpadButton_StickL) && v.discovery == LobbyDiscoveryLegacy && !v.appointment_game_id) {
            if (can_change_provider(&v)) {
                v.provider_panel = true;
                v.provider_choice = lobby_provider_index(v.config.provider);
                if (v.provider_choice < 0) v.provider_choice = 0;
                v.selection = (LobbyRoomId){0}; v.sel = -1;
            } else {
                const char *reason = v.config_state == RelayConfigFileError ?
                    TR("No se puede leer la red", "Cannot read network settings") :
                    v.config_state == RelayConfigInvalid ? TR("Ajustes de red no válidos", "Invalid network settings") :
                    !v.status_valid ? TR("Estado del complemento no disponible.", "Plugin status unavailable.") :
                    TR("Sal de la sala y cancela la búsqueda.", "Leave the room and cancel matching first.");
                snprintf(v.message, sizeof(v.message), "%s", reason);
            }
            draw(&v);
            last_draw = now;
            dirty = false;
            continue;
        }
        const int shown = v.count > 0 ? v.count : 0;
        const bool can_pick = v.status_valid && v.config_state == RelayConfigReady && shown > 0 && !v.private_panel && !v.private_room && !v.external_confirm &&
                              (!supports_reserved(&v) || v.match_available) && !show_matching(&v) &&
                              v.st.state != StateInRoom && v.st.state != StateConnecting;
        v.sel = can_pick ? lobby_room_index_on(v.selection, v.groups, v.sources, (size_t)shown) : -1;
        if (v.sel >= 0 && lobby_room_full(&v.groups[v.sel])) {
            v.sel = -1;
            v.selection = (LobbyRoomId){0};
            down &= ~HidNpadButton_A;
            snprintf(v.message, sizeof(v.message), "%s", ui_text("This room is full."));
            dirty = true;
        }
        if (v.selection.code[0] && v.sel < 0) {
            v.selection = (LobbyRoomId){0};
            if (down & HidNpadButton_A) {
                down &= ~HidNpadButton_A;
                snprintf(v.message, sizeof(v.message), "%s", TR("Esa sala ya no está en esta página.", "That room is no longer on this page."));
            }
        }
        if (card >= 0 && card < LOBBY_PAGE_SIZE) {
            dirty = true;
            int touched = can_pick ? lobby_room_index_on(g_touched_room, v.groups, v.sources, (size_t)shown) : -1;
            if(touched>=0 && v.node_directory &&
               (g_touched_node.node_id!=v.node_rows[touched].node_id || g_touched_node.flags!=v.node_rows[touched].flags ||
                memcmp(&g_touched_node.locator,&v.node_rows[touched].locator,sizeof(NodeRoomLocator))))touched=-1;
            if (touched < 0) {
                snprintf(v.message, sizeof(v.message), "%s", TR("No se puede elegir esa sala ahora.", "That room cannot be selected now."));
            } else if (lobby_room_full(&v.groups[touched])) {
                snprintf(v.message, sizeof(v.message), "%s", ui_text("This room is full."));
            } else if (v.sel == touched) {
                down |= HidNpadButton_A;
            } else {
                v.sel = touched;
                v.selection = lobby_room_id_on(&v.groups[v.sel], v.sources[v.sel]);
            }
        }
        if (can_pick && (down & HidNpadButton_AnyDown)) {
            v.sel = lobby_next_selectable_room(v.groups, shown, v.sel, 1);
            v.selection = v.sel >= 0 ? lobby_room_id_on(&v.groups[v.sel], v.sources[v.sel]) : (LobbyRoomId){0};
        }
        if (can_pick && v.sel >= 0 && (down & HidNpadButton_AnyUp)) {
            v.sel = lobby_next_selectable_room(v.groups, shown, v.sel, -1);
            v.selection = v.sel >= 0 ? lobby_room_id_on(&v.groups[v.sel], v.sources[v.sel]) : (LobbyRoomId){0};
        }

#if !RELAY_PROFILE_FIXED
        if ((down & HidNpadButton_R) && !v.appointment_game_id) {
            if (v.status_valid && v.st.state != StateInRoom && v.st.state != StateConnecting && edit_server(&v)) {
                v.page_index = 0;
                mutexLock(&g_list_lock);
                g_list_discovery = LobbyDiscoveryUnknown;
                mutexUnlock(&g_list_lock);
                restart_group_list(0, 0, thread_ok);
                v.count = thread_ok ? ListConnecting : ListUnavailable;
                v.next_page = LOBBY_PAGE_END;
                v.selection = (LobbyRoomId){0};
                v.sel = -1;
            }
        } else
#endif
        if (v.external_confirm) {
            if (down & HidNpadButton_B) {
                if (v.pinned.provider[0]) {
                    cancel_match(&v);
                    if (!v.pinned.code[0]) v.external_confirm = false;
                } else {
                    v.external_confirm = false; v.pinned = (LobbyRoomId){0};
                    memset(&v.pinned_group, 0, sizeof(v.pinned_group)); v.message[0] = '\0';
                }
            } else if ((down & HidNpadButton_A) && !external_room_expired(&v)) {
                if (open_selected_game(&v)) break;
                snprintf(v.message, sizeof(v.message), "%s", external_join_hint(v.pinned.provider, false));
            }
        } else if ((down & HidNpadButton_Y) && can_change_host_region(&v)) {
            if(v.node_picker_available){
                uint32_t selected=v.host_node;
                if(nodes_ui_pick(&pad,&g_lanywhere,v.host_node,&selected)){
                    v.host_node=selected;v.host_region=selected?RelayRegionCommunity:RelayRegionDefault;v.message[0]=0;
                    mutexLock(&g_list_lock);g_nodes_directory=true;mutexUnlock(&g_list_lock);
                    v.page_index=0;v.sel=-1;v.selection=(LobbyRoomId){0};
                v.count = thread_ok ? ListConnecting : ListUnavailable;
                v.next_page = LOBBY_PAGE_END;
                restart_group_list(0,0,thread_ok);
                }
                g_zh=ui_chinese();dirty=true;next_status=0;
            } else {
            const u32 region = next_host_region(&v);
            if (region == RelayRegionCommunity) draw_busy(ui_text("Checking community connection…"));
            const Result result = lanywhere_set_host_region(region);
            if (R_SUCCEEDED(result)) {
                v.host_region = region;
                v.message[0] = '\0';
            } else {
                snprintf(v.message, sizeof(v.message), "%s",
                    result == MAKERESULT(450, RELAY_COMMUNITY_UNREACHABLE_DESCRIPTION) ?
                    ui_text("Could not connect to the community line.") : ui_text("Could not change hosting line."));
            }
            }
        } else if ((down & HidNpadButton_Y) && can_share_game_password(&v)) {
            const char *message = room_password_edit(&g_lanywhere, v.st.code,
                has_direct_host(&v) || (v.capabilities & ROOM_ACCESS_CAPABILITY) != 0, &v.password_edit);
            if (message) snprintf(v.message, sizeof(v.message), "%s", message);
        } else if (!v.appointment_game_id && v.status_valid && v.config_state == RelayConfigReady && !show_matching(&v) && (down & HidNpadButton_Y) && v.discovery == LobbyDiscoveryLegacy && !is_self_network(&v) && !lobby_provider_is_lanplay(v.config.provider) && !v.private_room && v.st.state != StateInRoom && v.st.state != StateConnecting) {
            v.private_panel = true;
            v.selection = (LobbyRoomId){0};
            v.sel = -1;
            v.message[0] = '\0';
        } else if (v.private_panel && !v.private_room && (down & HidNpadButton_B)) {
            v.private_panel = false;
            v.message[0] = '\0';
        } else if (!v.appointment_game_id && !v.private_panel && (down & HidNpadButton_L)) {
            if (show_matching(&v)) {
                cancel_match(&v);
            } else if (v.match_available && supports_reserved(&v)) {
                start_quick(&v);
            }
        } else if (show_matching(&v)) {
            if (down & HidNpadButton_B) {
                cancel_match(&v);
            } else if ((down & HidNpadButton_A) && can_enter_game_password(&v)) {
                const char *message = room_password_unlock(&g_lanywhere, &v.password);
                if (message) snprintf(v.message, sizeof(v.message), "%s", message);
            } else if (v.match_available && supports_reserved(&v) && (down & HidNpadButton_A)) {
                if (v.match.mode == LobbyQuick && (v.match.state == MatchExpired || v.match.state == MatchError))
                    start_quick(&v);
                else if (v.match.mode == LobbyExact && v.match.state >= MatchWaitScan && v.match.state <= MatchAssigned && open_selected_game(&v))
                    break;
            }
        } else if (!v.private_panel && !v.private_room && (((down & HidNpadButton_ZL) && v.page_index > 0) ||
                   ((down & HidNpadButton_ZR) && v.next_page != LOBBY_PAGE_END && v.page_index + 1 < MAX_PAGE_HISTORY))) {
            if ((down & HidNpadButton_ZL) && v.page_index > 0)
                v.page_index--;
            else
                page_offsets[++v.page_index] = v.next_page;
            restart_group_list(page_offsets[v.page_index], v.page_index ? v.unified.snapshot : 0, thread_ok);
            v.count = thread_ok ? ListConnecting : ListUnavailable;
            v.next_page = LOBBY_PAGE_END;
            v.selection = (LobbyRoomId){0};
            v.sel = -1;
        } else if (v.sel >= 0 && (down & HidNpadButton_B)) {
            v.selection = (LobbyRoomId){0};
            v.sel = -1;
        } else if (v.sel >= 0 && (down & HidNpadButton_A)) {
            if(v.selection.code[0]){
                const int index=v.node_directory?lobby_room_index_on(v.selection,v.groups,v.sources,(size_t)v.count):-1;
                const bool gateway=index>=0&&v.node_rows[index].flags==NodeRoomGateway;
                if(index<0 || nodes_ui_check_room(&pad,gateway?0:v.node_rows[index].node_id,gateway,
                                                   gateway ? NULL : &v.node_rows[index].locator))select_room(&v);
                g_zh=ui_chinese();dirty=true;next_status=0;
            }
        } else if (v.private_room && (down & HidNpadButton_B)) {
            const Result result = lanywhere_leave();
            snprintf(v.message, sizeof(v.message), "%s", R_SUCCEEDED(result) ?
                TR("Saliste de la sala privada.", "You left the private room.") :
                ui_text("Could not leave the room. Try again."));
            poll_lanywhere(&v);
        } else if (v.discovery == LobbyDiscoveryLegacy && v.status_valid && v.private_panel && !is_self_network(&v) && !lobby_provider_is_lanplay(v.config.provider) && v.config_state == RelayConfigReady && v.st.state != StateInRoom && v.st.state != StateConnecting && (down & HidNpadButton_A)) {
            draw_busy(TR("Creando la sala…", "Creating the room…"));
            v.message[0] = '\0';
            if (R_FAILED(lanywhere_create(0)))
                snprintf(v.message, sizeof(v.message), "%s", TR("No se pudo crear la sala. ¿Hay internet?", "Could not create the room. Is there internet?"));
            else
                snprintf(v.message, sizeof(v.message), "%s", TR("Sala creada: pásale el código a tu amigo.", "Share the code with your friend."));
        } else if (v.discovery == LobbyDiscoveryLegacy && v.status_valid && v.private_panel && !is_self_network(&v) && !lobby_provider_is_lanplay(v.config.provider) && v.config_state == RelayConfigReady && v.st.state != StateInRoom && v.st.state != StateConnecting && (down & HidNpadButton_X)) {
            char code[16] = {0};
            if (ask_code(code, sizeof(code))) {
                char busy[64];
                snprintf(busy, sizeof(busy), TR("Entrando en la sala %s…", "Joining room %s…"), code);
                draw_busy(busy);
                v.message[0] = '\0';
                if (R_FAILED(lanywhere_join(0, code)))
                    snprintf(v.message, sizeof(v.message), TR("No se pudo entrar en la sala %s.", "Could not join room %s."), code);
                else
                    snprintf(v.message, sizeof(v.message), TR("Estás en la sala %s. Ya puedes abrir el juego.", "Joined %s. Open the game."), code);
            } else if (g_keyboard_error) {
                snprintf(v.message, sizeof(v.message), "%s", ui_text(g_keyboard_error));
            }
        } else if (can_change_visibility(&v) && (down & HidNpadButton_X)) {
            const bool visible = !v.config.public_groups;
            if (!ensure_config_dir() || !relay_config_save_public(RELAY_CONFIG_PATH, visible)) {
                config_write_error(&v);
            } else {
                v.config.public_groups = visible;
                snprintf(v.message, sizeof(v.message), "%s", visible ? TR("Tus grupos se verán en este servidor.", "Next game room: public.") :
                                                                       TR("Tus grupos solo se verán en la red local.", "Next game room: local only."));
            }
        }
        if (down & (HidNpadButton_A | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_B)) {
            poll_lanywhere(&v);
            v.room_notice_visible = show_room_notice(&v, now_ms());
            next_status = now_ms() + STATUS_POLL_MS;
            ueventSignal(&g_refresh);
        }

        // Solo se redibuja si algo cambió (o cada pocos segundos): dibujar en software gasta batería.
        now = now_ms();
        if (thread_ok && down && update_room_password(&v, now)) dirty = true;
        bool status_changed = v.status_valid != last.status_valid || v.room_notice_visible != last.room_notice_visible ||
                              v.host_region != last.host_region || v.host_regions != last.host_regions ||
                              v.st.state != last.st.state || v.st.member_count != last.st.member_count ||
                              strcmp(v.st.code, last.st.code) != 0 || strcmp(v.st.error, last.st.error) != 0 ||
                              v.match_available != last.match_available || memcmp(&v.match, &last.match, sizeof(v.match)) != 0 ||
                              memcmp(&v.direct_host, &last.direct_host, sizeof(v.direct_host)) != 0;
        if (down || status_changed || strcmp(v.message, last.message) != 0)
            dirty = true;
        if (dirty || now - last_draw >= 5000 || (g_board_title_animates && now - last_draw >= 66)) {
            draw(&v);
            last = v;
            last.password_edit = (RoomPasswordEdit){0};
            last_draw = now;
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }

    mutexLock(&g_list_lock);
    g_quit = true;
    mutexUnlock(&g_list_lock);
    ueventSignal(&g_refresh);
    if (thread_ok)
        threadWaitForExit(&thread);
    if (thread_created)
        threadClose(&thread);
    serviceClose(&g_lanywhere_list);
    for (int i = 0; i < g_game_count; i++)
        free(g_games[i].icon.px);
    if (inventory_started) threadWaitForExit(&inventory);
    if (inventory_created) threadClose(&inventory);
    if (g_ns_ready) nsExit();
    serviceClose(&g_lanywhere);
    nodes_ui_shutdown();
    update_ui_shutdown();
    gfx_exit();
    return 0;
}
