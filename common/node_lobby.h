/* Public-node discovery and optional temporary group measurement. GPL-2.0-only */
#ifndef LANYWHERE_NODE_LOBBY_H
#define LANYWHERE_NODE_LOBBY_H
#include <stddef.h>
#include <stdint.h>

#define NODE_LOBBY_CAPABILITY 32768u
#define NODE_CATALOG_REQUEST 0x32u
#define NODE_CATALOG_RESPONSE 0xb2u
#define NODE_LOBBY_REQUEST 0x33u
#define NODE_LOBBY_RESPONSE 0xb3u
#define NODE_FRIEND_REQUEST 0x34u
#define NODE_FRIEND_RESPONSE 0xb4u
#define NODE_REALM_LOCATORS_REQUEST 0x35u
#define NODE_REALM_LOCATORS_RESPONSE 0xb5u
#define NODE_LOBBY_PAGE_SIZE 6u
#define NODE_LOBBY_ROW_SIZE 408u
#define NODE_FRIEND_MAX_NODES 13u
#define NODE_FRIEND_MAX_PEOPLE 8u
#define NODE_FRIEND_REQUEST_SIZE 240u
#define NODE_FRIEND_RESPONSE_SIZE 292u
#define NODE_LOBBY_END UINT32_MAX

enum NodeRoomFlags { NodeRoomDirect = 1u, NodeRoomGateway = 2u };
enum NodeFlavor { NodeFlavorNone = 0, NodeFlavorStandard = 1, NodeFlavorFork = 2 };
enum NodeFriendOperation { NodeFriendCreate = 1, NodeFriendJoin, NodeFriendReport, NodeFriendPoll, NodeFriendLeave };
enum NodeFriendStatus { NodeFriendOk, NodeFriendExpired, NodeFriendInvalid, NodeFriendFull, NodeFriendBusy };
enum NodeProbeKind { NodeProbeTcp = 1, NodeProbeUdp = 2 };
enum NodeProbeStatus { NodeProbePending, NodeProbeMeasured, NodeProbeUnreachable };

typedef struct {
    uint8_t network_id[32];
    uint8_t bssid[6];
    uint8_t flavor;
    uint8_t reserved0;
    uint32_t host_ip;
    char code[8];
    uint8_t reserved[12];
} NodeRoomLocator;

typedef struct {
    uint8_t group[336];
    uint32_t node_id;
    uint32_t flags;
    NodeRoomLocator locator;
} NodeRoomWire;

typedef struct {
    uint64_t game_id;
    uint32_t node_id;
    uint32_t reserved;
    NodeRoomLocator locator;
} NodeRoomSelection;

typedef struct {
    uint32_t offset;
    uint32_t snapshot;
    uint64_t game_id;
    uint32_t node_mask;
    uint32_t reserved;
} NodeRoomsRequest;

typedef struct {
    uint32_t node_id;
    uint16_t median_ms;
    uint16_t jitter_ms;
    uint8_t sent;
    uint8_t received;
    uint8_t kind;
    uint8_t status;
    uint16_t age_seconds;
    uint16_t reserved;
} NodeProbeSummary;

typedef struct {
    uint32_t operation;
    char code[8];
    uint8_t participant[16];
    uint32_t count;
    NodeProbeSummary samples[NODE_FRIEND_MAX_NODES];
} NodeFriendRequest;

typedef struct {
    uint32_t node_id;
    uint32_t worst_median_ms;
    uint32_t worst_jitter_ms;
    uint8_t measured_members;
    uint8_t reachable_members;
    uint8_t minimum_sent;
    uint8_t minimum_received;
    uint8_t kind;
    uint8_t recommendation_rank;
    uint16_t reserved;
} NodeFriendSummary;

typedef struct {
    uint32_t status;
    char code[8];
    uint8_t participant[16];
    uint16_t remaining_seconds;
    uint8_t participants;
    uint8_t reported_participants;
    NodeFriendSummary nodes[NODE_FRIEND_MAX_NODES];
} NodeFriendResponse;

#if defined(__cplusplus)
#define NODE_STATIC_ASSERT(expr, text) static_assert(expr, text)
#else
#define NODE_STATIC_ASSERT(expr, text) _Static_assert(expr, text)
#endif
NODE_STATIC_ASSERT(sizeof(NodeRoomLocator) == 64, "node locator layout");
NODE_STATIC_ASSERT(sizeof(NodeRoomWire) == NODE_LOBBY_ROW_SIZE, "node room row layout");
NODE_STATIC_ASSERT(sizeof(NodeRoomsRequest) == 24, "node request IPC layout");
NODE_STATIC_ASSERT(sizeof(NodeRoomSelection) == 80, "node selection IPC layout");
NODE_STATIC_ASSERT(offsetof(NodeRoomsRequest, node_mask) == 16, "node request wire prefix");
NODE_STATIC_ASSERT(sizeof(NodeProbeSummary) == 16, "node probe summary layout");
NODE_STATIC_ASSERT(sizeof(NodeFriendRequest) == NODE_FRIEND_REQUEST_SIZE, "friend request layout");
NODE_STATIC_ASSERT(sizeof(NodeFriendSummary) == 20, "friend summary layout");
NODE_STATIC_ASSERT(sizeof(NodeFriendResponse) == NODE_FRIEND_RESPONSE_SIZE, "friend response layout");
#undef NODE_STATIC_ASSERT
#endif
