/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
#include "ldn.h"
#include "../../../common/direct_room_metadata.h"
#include "../../../common/game_identity_text.h"
#include "../../../common/game_update_compatibility.h"
#include "../../../common/list_status.h"
#include "../../../common/node_catalog.h"
#include "../../../common/roomcircuit_paths.h"
#include <switch/sf/tipc.h>
#include <stdlib.h>
#include <string.h>

/* Atmosphere SM extension ABI, as in Atmosphere-NX sm_ams.os.horizon.c
 * (GPL-2.0): 65004 HasMitm, 65100 HasService; the result is one byte. */
Result lanywhereOverlayHasService(bool *present, bool config) {
    *present = false;
    u8 value = 0;
    const SmServiceName name = smEncodeName(config ? "ldn:u" : "lanywhr");
    Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), config ? 65004 : 65100, name, value);
    if (R_SUCCEEDED(rc)) *present = (value & 1) != 0;
    return rc;
}

Result ldnMitmGetConfigFromService(Service* s, LdnMitmConfigService *out) {
    return serviceDispatch(s, 65000,
        .out_num_objects = 1,
        .out_objects = &out->s
    );
}

Result ldnMitmGetLogging(LdnMitmConfigService *s, u32 *enabled) {
    *enabled = 0;
    u32 value = 0;
    Result rc = serviceDispatchOut(&s->s, 65002, value);
    if (R_SUCCEEDED(rc)) *enabled = value != 0;
    return rc;
}

Result ldnMitmSetLogging(LdnMitmConfigService *s, u32 enabled) {
    return serviceDispatchIn(&s->s, 65003, enabled);
}

Result ldnMitmGetEnabled(LdnMitmConfigService *s, u32 *enabled) {
    *enabled = 0;
    u32 value = 0;
    Result rc = serviceDispatchOut(&s->s, 65004, value);
    if (R_SUCCEEDED(rc)) *enabled = value != 0;
    return rc;
}

Result ldnMitmSetEnabled(LdnMitmConfigService *s, u32 enabled) {
    return serviceDispatchIn(&s->s, 65005, enabled);
}

Result ldnMitmGetVersion(LdnMitmConfigService *s, char *version) {
    char version_s[32] = {0};
    memset(version, 0, 32);
    Result rc = serviceDispatchOut(&s->s, 65001, version_s);
    if (R_SUCCEEDED(rc)) {
        memcpy(version, version_s, 31);
        version[31] = '\0';
    }
    return rc;
}

Result lanywhereOverlayGetProtocol(Service *s, u32 *version) {
    *version = 0;
    return serviceDispatchOut(s, 10, *version);
}
Result lanywhereOverlayGetStorageSelection(Service *s, u32 *selection) {
    *selection = 2;
    u32 value = 2;
    Result rc = serviceDispatchOut(s, 46, value);
    if (R_FAILED(rc)) return rc;
    if (value != 0 && value != 1 && value != 3) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *selection = value;
    return 0;
}

Result lanywhereOverlayGetStorageHealth(Service *s, u32 *health) {
    *health = RC_STORAGE_HEALTH_LEGACY_UNCHECKED;
    u32 value = 0;
    Result rc = serviceDispatchOut(s, RC_STORAGE_HEALTH_IPC, value);
    if (R_FAILED(rc)) return rc;
    if (value & ~(RC_STORAGE_HEALTH_LEGACY_CHANGED | RC_STORAGE_HEALTH_LEGACY_UNCHECKED |
                  RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT | RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *health = value;
    return 0;
}

Result lanywhereOverlayGetNodeCatalog(Service *s, NodeDynamicCatalog *out) {
    memset(out, 0, sizeof(*out));
    Result rc = serviceDispatch(s, NODE_GET_CATALOG_IPC,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { out, sizeof(*out) } });
    if (R_FAILED(rc)) { memset(out, 0, sizeof(*out)); return rc; }
    if (out->version != 1 || out->count > sizeof(out->nodes) / sizeof(out->nodes[0])) goto invalid;
    for (u32 i = 0; i < out->count; ++i) {
        const NodeDynamicEntry *node = &out->nodes[i];
        if (!((node->id >= 13 && node->id < NODE_SHARED_CAPACITY) ||
              (node->id >= NODE_LOCAL_BASE && node->id < NODE_LOCAL_BASE + NODE_LOCAL_CAPACITY)) ||
            node->protocol != 2 || node->region > 5 || node->flags > 1 || !node->port ||
            node->reserved0[0] || node->reserved0[1] || !node->name[0] || !node->host[0] ||
            !memchr(node->name, '\0', sizeof(node->name)) || !memchr(node->host, '\0', sizeof(node->host)))
            goto invalid;
        for (u32 j = 0; j < i; ++j)
            if (out->nodes[j].id == node->id) goto invalid;
    }
    return 0;
invalid:
    memset(out, 0, sizeof(*out));
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

Result lanywhereOverlayStartNodeTask(Service *s, const NodeDynamicEntry *expected, u64 *task_id) {
    if (!expected || !task_id) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *task_id = 0;
    Result rc = serviceDispatchOut(s, NODE_START_HOST_TASK_IPC, *task_id,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { (void *)expected, sizeof(*expected) } });
    if (R_FAILED(rc)) { *task_id = 0; return rc; }
    if (!*task_id) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return 0;
}

Result lanywhereOverlayGetNodeTask(Service *s, u64 task_id, NodeHostTaskStatus *out) {
    if (!out) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(out, 0, sizeof(*out));
    Result rc = serviceDispatchIn(s, NODE_GET_HOST_TASK_IPC, task_id,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { out, sizeof(*out) } });
    if (R_FAILED(rc)) { memset(out, 0, sizeof(*out)); return rc; }
    const NodeDynamicEntry *node = &out->identity;
    if (out->version != 1 || out->state > NodeHostTaskExpired || out->reserved ||
        ((out->state == NodeHostTaskWorking || out->state == NodeHostTaskFinished) &&
         (!out->task_id || (task_id && out->task_id != task_id) ||
          !((node->id < NODE_SHARED_CAPACITY) ||
            (node->id >= NODE_LOCAL_BASE && node->id < NODE_LOCAL_BASE + NODE_LOCAL_CAPACITY)) ||
          node->protocol > 2 || node->region > 5 || node->flags > 1 || !node->port ||
          node->reserved0[0] || node->reserved0[1] || !node->name[0] || !node->host[0] ||
          !memchr(node->name, '\0', sizeof(node->name)) || !memchr(node->host, '\0', sizeof(node->host))))) {
        memset(out, 0, sizeof(*out));
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    return 0;
}

Result lanywhereOverlayGetHostIdentity(Service *s, NodeDynamicEntry *out) {
    memset(out, 0, sizeof(*out));
    Result rc = serviceDispatch(s, NODE_GET_HOST_IDENTITY_IPC,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { out, sizeof(*out) } });
    if (R_FAILED(rc)) { memset(out, 0, sizeof(*out)); return rc; }
    if (!((out->id < NODE_SHARED_CAPACITY) ||
          (out->id >= NODE_LOCAL_BASE && out->id < NODE_LOCAL_BASE + NODE_LOCAL_CAPACITY)) ||
        out->protocol > 2 || out->region > 5 || out->flags > 1 || !out->port ||
        out->reserved0[0] || out->reserved0[1] || !out->name[0] || !out->host[0] ||
        !memchr(out->name, '\0', sizeof(out->name)) || !memchr(out->host, '\0', sizeof(out->host))) {
        memset(out, 0, sizeof(*out));
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    return 0;
}
Result lanywhereOverlayGetStatus(Service *s, LanywhereOverlayStatus *out) {
    memset(out, 0, sizeof(*out));
    LanywhereOverlayStatus value = {0};
    Result rc = serviceDispatchOut(s, 3, value);
    if (R_FAILED(rc)) return rc;
    if (value.state > 3 || value.member_count > 8 || (value.state == 2 && value.my_index >= 8))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    value.code[6] = value.code[7] = '\0';
    value.error[63] = '\0';
    *out = value;
    return 0;
}
Result lanywhereOverlayGetMatch(Service *s, LanywhereOverlayMatch *out) {
    memset(out, 0, sizeof(*out));
    LanywhereOverlayMatch value = {0};
    Result rc = serviceDispatchOut(s, 9, value);
    if (R_FAILED(rc)) return rc;
    if (value.state > 8 || value.mode > 2 || value.players > 8 || value.target > 8)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    value.room[6] = value.room[7] = '\0';
    value.error[63] = '\0';
    *out = value;
    return 0;
}

Result lanywhereOverlayGetHostNode(Service *s, NodeHostInfo *out) {
    memset(out, 0, sizeof(*out));
    NodeHostInfo value = {0};
    Result rc = serviceDispatch(s, NODE_GET_HOST_INFO_IPC,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { &value, sizeof(value) } });
    if (R_FAILED(rc)) return rc;
    if (value.version != 1 || value.status > NodeHostSelected || value.reserved ||
        !memchr(value.name, '\0', sizeof(value.name)) || !value.name[0])
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out = value;
    return 0;
}

Result lanywhereOverlayGetActiveRoute(Service *s, NodeActiveRouteInfo *out) {
    memset(out, 0, sizeof(*out));
    NodeActiveRouteInfo value = {0};
    Result rc = serviceDispatch(s, NODE_GET_ACTIVE_ROUTE_IPC,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { &value, sizeof(value) } });
    if (R_FAILED(rc)) return rc;
    if (value.version != 1 || value.status > NodeRouteUnavailable || value.reserved ||
        !memchr(value.name, '\0', sizeof(value.name)) ||
        (value.status != NodeRouteNone && !value.name[0]))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out = value;
    return 0;
}

Result lanywhereOverlayGetPasscodeHostState(Service *s, RoomPasscodeHostState *out) {
    memset(out, 0, sizeof(*out));
    RoomPasscodeHostState value = {0};
    Result rc = serviceDispatchOut(s, ROOM_PASSCODE_HOST_STATE_IPC, value);
    if (rc == MAKERESULT(10, 221)) {
        RoomPasscodeHost legacy = {0};
        rc = serviceDispatchOut(s, ROOM_PASSCODE_IPC_HOST, legacy);
        if (R_FAILED(rc)) return rc;
        if (legacy.reserved) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        value.status = legacy.status;
        value.mode = OVERLAY_ROOM_ACCESS_UNKNOWN;
        memcpy(value.room, legacy.room, sizeof(value.room));
    } else {
        if (R_FAILED(rc)) return rc;
        if (value.mode > RoomAccessHidden) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (value.status > RoomPasscodeBusy)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (value.status == RoomPasscodeOk) {
        if (value.room[6] || value.room[7])
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        for (size_t i = 0; i < 6; ++i) {
            const char c = value.room[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
                return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        *out = value;
    } else {
        out->status = value.status;
    }
    return 0;
}

Result lanywhereOverlaySetRoomAccess(Service *s, const RoomAccessChange *change, u32 *status) {
    *status = RoomPasscodeUnavailable;
    u32 value = RoomPasscodeUnavailable;
    Result rc = serviceDispatchInOut(s, ROOM_ACCESS_IPC_SET, *change, value);
    if (R_FAILED(rc)) return rc;
    if (value > RoomPasscodeBusy) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *status = value;
    return 0;
}

enum { ListNodeRoomsIpc = 37, SelectNodeRoomIpc = 38 };

/* The existing GroupSummaryV2 IPC reply inside NodeRoomWire. */
typedef struct {
    char code[8];
    u64 game_id, application_id, program_id;
    u8 players, max_players, metadata_flags, access, reserved[4];
    char host[40], title_en[128], title_zh[128];
} RoomSummary;
_Static_assert(sizeof(RoomSummary) == sizeof(((NodeRoomWire *)0)->group) &&
               offsetof(RoomSummary, title_en) == 80 && offsetof(RoomSummary, title_zh) == 208,
               "overlay sees the published room-summary layout");

static bool room_code_valid(const char code[8]) {
    if (code[6] || code[7]) return false;
    for (unsigned i = 0; i < 6; ++i)
        if (!((code[i] >= 'A' && code[i] <= 'Z' && code[i] != 'I' && code[i] != 'O') ||
              (code[i] >= '2' && code[i] <= '9'))) return false;
    return true;
}

static bool room_locator_valid(const NodeRoomSelection *selection, u32 flags) {
    const u32 node = selection->node_id;
    const NodeRoomLocator *locator = &selection->locator;
    if (node >= NODE_SHARED_CAPACITY || selection->reserved || !selection->game_id ||
        !room_code_valid(locator->code) || locator->reserved0) return false;
    const bool custom = node >= NODE_CATALOG_COUNT;
    for (size_t i = custom ? 4 : 0; i < sizeof(locator->reserved); ++i)
        if (locator->reserved[i]) return false;
    if (flags == NodeRoomGateway) return !custom;
    if (flags != NodeRoomDirect) return false;
    if (node < 2) return !locator->flavor && selection->game_id != UINT64_MAX;
    if (locator->flavor != NodeFlavorStandard && locator->flavor != NodeFlavorFork) return false;
    u64 advertised_id;
    memcpy(&advertised_id, locator->network_id, sizeof(advertised_id));
    return advertised_id == selection->game_id && (locator->host_ip >> 16) == 0x0a0d &&
        (!custom || memcmp(locator->reserved, (u8[4]){0}, 4));
}

static void copy_title(char out[128], const char title[128]) {
    const char *end = memchr(title, 0, 128);
    if (end) (void)game_identity_safe_title((const u8 *)title, (size_t)(end - title), out, 128);
}

Result lanywhereOverlayListRooms(Service *s, const NodeRoomsRequest *request, UnifiedPage *page,
                               OverlayPublicRoom rooms[NODE_LOBBY_PAGE_SIZE]) {
    const Result invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *page = (UnifiedPage){ .count = -1, .next = NODE_LOBBY_END, .status = ListStatusProtocol };
    memset(rooms, 0, NODE_LOBBY_PAGE_SIZE * sizeof(*rooms));
    if (!request || request->reserved || (request->offset && !request->snapshot) ||
        request->offset > UINT32_MAX - NODE_LOBBY_PAGE_SIZE ||
        !request->node_mask || (request->node_mask >> NODE_SHARED_CAPACITY)) return invalid;

    union {
        VersionedNodeRoomWire versioned[NODE_LOBBY_PAGE_SIZE];
        NodeRoomWire plain[NODE_LOBBY_PAGE_SIZE];
    } wire = {0};
    UnifiedPage result = *page;
    Result rc = serviceDispatchInOut(s, ROOM_GAME_VERSION_LIST_IPC, *request, result,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { wire.versioned, sizeof(wire.versioned) } });
    const bool versioned = rc != MAKERESULT(10, 221);
    if (!versioned) {
        memset(&wire, 0, sizeof(wire));
        result = *page;
        rc = serviceDispatchInOut(s, DIRECT_ROOM_LIST_IPC, *request, result,
            .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
            .buffers = { { wire.plain, sizeof(wire.plain) } });
        if (rc == MAKERESULT(10, 221)) {
            memset(&wire, 0, sizeof(wire));
            result = *page;
            rc = serviceDispatchInOut(s, ListNodeRoomsIpc, *request, result,
                .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
                .buffers = { { wire.plain, sizeof(wire.plain) } });
        }
    }
    if (R_FAILED(rc)) return rc;
    if (result.count == -1 && (lanywhere_list_status_valid(result.status) || result.status == UnifiedListStale)) {
        page->status = result.status;
        return 0;
    }
    if (result.count < 0 || result.count > (int32_t)NODE_LOBBY_PAGE_SIZE || result.status || !result.snapshot ||
        (request->snapshot && result.snapshot != request->snapshot) ||
        result.available > result.total || result.total > NODE_SHARED_CAPACITY ||
        (result.next != NODE_LOBBY_END && (!result.count || result.next != request->offset + (u32)result.count)))
        return invalid;
    for (int32_t i = 0; i < result.count; ++i) {
        const NodeRoomWire *row = versioned ? &wire.versioned[i].room : &wire.plain[i];
        RoomSummary group;
        memcpy(&group, row->group, sizeof(group));
        OverlayPublicRoom room = {
            .selection = { .game_id = group.game_id, .node_id = row->node_id, .locator = row->locator },
            .flags = row->flags, .players = group.players, .max_players = group.max_players, .access = group.access,
        };
        if (!room_locator_valid(&room.selection, room.flags) ||
            !(request->node_mask & (1u << room.selection.node_id)) ||
            (request->game_id && request->game_id != room.selection.game_id) ||
            memcmp(group.code, room.selection.locator.code, sizeof(group.code)) ||
            !room.players || room.players > room.max_players || room.max_players > 8 ||
            room.access > RoomAccessHidden ||
            (room.selection.node_id && room.access &&
             (room.selection.node_id < 2 || room.flags != NodeRoomDirect))) {
            memset(rooms, 0, NODE_LOBBY_PAGE_SIZE * sizeof(*rooms));
            return invalid;
        }
        copy_title(room.title_en, group.title_en);
        copy_title(room.title_zh, group.title_zh);
        /* The app's local-name/cache identity bit is not a host title ID. */
        if (versioned && group.metadata_flags == 1 && group.application_id && group.application_id != UINT64_MAX &&
            room_game_version_valid(wire.versioned[i].game_version, false)) {
            room.application_id = group.application_id;
            memcpy(room.game_version, wire.versioned[i].game_version, sizeof(room.game_version));
        }
        rooms[i] = room;
    }
    *page = result;
    return 0;
}

Result lanywhereOverlaySelectRoom(Service *s, const OverlayPublicRoom *room) {
    if (!room || !room_locator_valid(&room->selection, room->flags) ||
        !room->players || room->players >= room->max_players || room->max_players > 8 ||
        room->access > RoomAccessHidden || (!room->selection.node_id && room->access == RoomAccessHidden))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (room->flags == NodeRoomDirect)
        return serviceDispatchIn(s, SelectNodeRoomIpc, room->selection);
    UnifiedRoomRequest request = { .game_id = room->selection.game_id };
    const char *provider = LanywhereNodes[room->selection.node_id].provider;
    memcpy(request.code, room->selection.locator.code, sizeof(request.code));
    memcpy(request.provider, provider, strlen(provider) + 1);
    return serviceDispatchIn(s, UNIFIED_LOBBY_IPC_SELECT, request);
}

Result lanywhereOverlayGetHostVersionTarget(Service *s, RoomGameVersionTarget *target) {
    *target = (RoomGameVersionTarget){0};
    return serviceDispatchOut(s, ROOM_GAME_VERSION_HOST_IPC, *target);
}

Result lanywhereOverlayPublishHostVersion(Service *s, const RoomGameVersionUpdate *update) {
    if (!update->target.application_id || !update->target.generation ||
        !room_game_version_valid(update->game_version, false))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return serviceDispatchIn(s, ROOM_GAME_VERSION_PUBLISH_IPC, *update);
}

/* Only called by the room worker and never from overlay initialization. */
bool lanywhereOverlayReadGameVersion(u64 application_id, u64 required_lcid, char version[ROOM_GAME_VERSION_SIZE]) {
    memset(version, 0, ROOM_GAME_VERSION_SIZE);
    if (!application_id || application_id == UINT64_MAX || required_lcid == UINT64_MAX ||
        R_FAILED(nsInitialize())) return false;
    const u64 alternate = required_lcid ? room_game_update_alternate(application_id) : 0;
    bool alternate_installed = false;
    if (alternate && (R_FAILED(nsIsAnyApplicationEntityInstalled(alternate, &alternate_installed)) ||
                      alternate_installed)) {
        nsExit();
        return false;
    }
    bool matched = false, installed = false;
    if (R_SUCCEEDED(nsIsAnyApplicationEntityInstalled(application_id, &installed)) && installed) {
        NsApplicationControlData *control = malloc(sizeof(*control));
        if (control) {
            u64 size = 0;
            if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_StorageOnly,
                    application_id, control, sizeof(*control), &size)) &&
                size >= sizeof(control->nacp) && size <= sizeof(*control)) {
                matched = !required_lcid;
                for (size_t i = 0; i < sizeof(control->nacp.local_communication_id) / sizeof(u64); ++i)
                    if (control->nacp.local_communication_id[i] == required_lcid) matched = true;
                const char *data = control->nacp.display_version;
                const char *end = memchr(data, 0, sizeof(control->nacp.display_version));
                if (matched && end && (size_t)(end - data) < ROOM_GAME_VERSION_SIZE)
                    memcpy(version, data, (size_t)(end - data));
            }
            free(control);
        }
    }
    nsExit();
    if (matched && room_game_version_valid(version, false)) return true;
    memset(version, 0, ROOM_GAME_VERSION_SIZE);
    return false;
}
