#pragma once

#include <stdbool.h>
#include "node_lobby.h"

#define ROOM_GAME_VERSION_CAPABILITY 1048576u
#define ROOM_GAME_VERSION_LIST_REQUEST 0x3au
#define ROOM_GAME_VERSION_LIST_RESPONSE 0xbau
#define ROOM_GAME_VERSION_SET_REQUEST 0x3bu
#define ROOM_GAME_VERSION_LIST_IPC 53u
#define ROOM_GAME_VERSION_HOST_IPC 54u
#define ROOM_GAME_VERSION_PUBLISH_IPC 55u
#define ROOM_GAME_VERSION_SIZE 16u

typedef struct {
    NodeRoomWire room;
    char game_version[ROOM_GAME_VERSION_SIZE];
} VersionedNodeRoomWire;

typedef struct {
    uint64_t application_id;
    uint64_t generation;
} RoomGameVersionTarget;

typedef struct {
    RoomGameVersionTarget target;
    char game_version[ROOM_GAME_VERSION_SIZE];
} RoomGameVersionUpdate;

static inline bool room_game_version_digit(char c) { return c >= '0' && c <= '9'; }

/* Game display metadata only. Never use this value for LDN compatibility. */
static inline bool room_game_version_valid(const char version[ROOM_GAME_VERSION_SIZE], bool allow_empty)
{
    size_t end = 0;
    while (end < ROOM_GAME_VERSION_SIZE && version[end]) ++end;
    if (end == ROOM_GAME_VERSION_SIZE) return false;
    for (size_t i = end; i < ROOM_GAME_VERSION_SIZE; ++i)
        if (version[i]) return false;
    if (!end) return allow_empty;
    const size_t first_digit = version[0] == 'v' || version[0] == 'V' ? 1 : 0;
    if (!room_game_version_digit(version[first_digit])) return false;
    for (size_t i = first_digit + 1; i < end; ++i) {
        const char c = version[i];
        if (!room_game_version_digit(c) && !(c >= 'A' && c <= 'Z') &&
            !(c >= 'a' && c <= 'z') && c != '.' && c != '_' && c != '+' && c != '-') return false;
    }
    return true;
}

#ifdef __cplusplus
#define RC_GAME_VERSION_LAYOUT static_assert
#else
#define RC_GAME_VERSION_LAYOUT _Static_assert
#endif
RC_GAME_VERSION_LAYOUT(sizeof(VersionedNodeRoomWire) == 424, "versioned node room layout");
RC_GAME_VERSION_LAYOUT(sizeof(RoomGameVersionTarget) == 16, "host game version target layout");
RC_GAME_VERSION_LAYOUT(sizeof(RoomGameVersionUpdate) == 32, "host game version update layout");
#undef RC_GAME_VERSION_LAYOUT
