#ifndef ROOMCIRCUIT_DIRECT_ROOM_METADATA_H
#define ROOMCIRCUIT_DIRECT_ROOM_METADATA_H

#include <stddef.h>
#include <stdint.h>
#include "room_passcode.h"

#define DIRECT_ROOM_CAPABILITY 131072u
#define DIRECT_ROOM_METADATA_REQUEST 0x36u
#define DIRECT_ROOM_METADATA_RESPONSE 0xb6u
#define DIRECT_ROOM_LIST_REQUEST 0x37u
#define DIRECT_ROOM_LIST_RESPONSE 0xb7u
#define DIRECT_ROOM_HOST_IPC 39u
#define DIRECT_ROOM_PASSCODE_IPC 40u
#define DIRECT_ROOM_LIST_IPC 41u

enum DirectRoomOperation { DirectRoomPublish = 1, DirectRoomRead = 2 };
enum DirectRoomHostFlags { DirectRoomHostActive = 1, DirectRoomHostCanEdit = 2 };

typedef struct {
    uint32_t operation;
    uint32_t node_id;
    uint32_t host_ip;
    uint8_t network_id[32];
    uint8_t bssid[6];
    uint8_t mode;
    uint8_t length;
    char password[ROOM_PASSCODE_MAX];
    uint8_t proof[32];
    uint32_t reserved_proof;
    uint64_t application_id;
    uint64_t program_id;
    char title_en[128];
    char title_zh[128];
} DirectRoomMetadata;

typedef struct {
    uint32_t status;
    uint8_t mode;
    uint8_t length;
    uint8_t reserved[2];
    char password[ROOM_PASSCODE_MAX];
} DirectRoomMetadataReply;

typedef struct {
    uint64_t epoch;
    uint32_t node_id;
    uint32_t flags;
    uint32_t host_ip;
    uint32_t reserved_ip;
    uint8_t network_id[32];
    uint8_t bssid[6];
    uint8_t reserved[2];
    uint64_t application_id;
    uint64_t program_id;
    char title_en[128];
    char title_zh[128];
} DirectRoomHost;

#ifdef __cplusplus
#define DIRECT_ROOM_LAYOUT static_assert
#else
#define DIRECT_ROOM_LAYOUT _Static_assert
#endif
DIRECT_ROOM_LAYOUT(sizeof(DirectRoomMetadata) == 384, "direct room registration");
DIRECT_ROOM_LAYOUT(offsetof(DirectRoomMetadata, password) == 52, "direct room lookup prefix");
DIRECT_ROOM_LAYOUT(offsetof(DirectRoomMetadata, application_id) == 112, "direct room game metadata");
DIRECT_ROOM_LAYOUT(sizeof(DirectRoomMetadataReply) == 32, "direct room reply");
DIRECT_ROOM_LAYOUT(sizeof(DirectRoomHost) == 336, "direct host IPC buffer");
#undef DIRECT_ROOM_LAYOUT
#endif
