/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
#include "ldn.h"
#include "../../../common/roomcircuit_paths.h"
#include <switch/sf/tipc.h>
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

Result lanywhereOverlayGetPasscodeHost(Service *s, RoomPasscodeHost *out) {
    memset(out, 0, sizeof(*out));
    RoomPasscodeHost value = {0};
    Result rc = serviceDispatchOut(s, ROOM_PASSCODE_IPC_HOST, value);
    if (R_FAILED(rc)) return rc;
    if (value.reserved || value.status > RoomPasscodeBusy)
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
