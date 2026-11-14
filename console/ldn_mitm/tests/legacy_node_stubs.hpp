/* Inactive direct-node seam for legacy source-slice regression fixtures.
 * New direct-node behavior is exercised by the transport and node-entry tests. */
#pragma once
#include <atomic>
#include <cassert>
#include "node_catalog.h"
#include "node_lobby.h"
[[maybe_unused]] static std::atomic<bool> g_direct_session{false};
[[maybe_unused]] static GameRoute g_direct_room{};
bool DirectNodeRoute(const DiscoveryRoute &route);
bool TrustedNodeEndpointLocked(const DiscoveryRoute &route);
namespace lanplay {
struct Endpoint { uint32_t ipv4; uint16_t port; };
struct Snapshot {
    bool active=false, hosting=false;
    uint32_t node=0, address=0, sent=0, received=0;
    uint64_t epoch=0, game=0;
    uint8_t member=0, members=0;
    char error[64]{};
};
inline Snapshot State() { return {}; }
inline bool Configure(uint32_t, const Endpoint &) { assert(false); return false; }
inline int Scan(const void *, size_t, const NodeRoomLocator *, void (*)(const char *, const void *, void *), void *) {
    assert(false); return -1;
}
inline void Publish(const void *, size_t) { assert(false); }
inline void Leave() { assert(false); }
}
[[maybe_unused]] static Result EnterDirectNode(bool, const char *, uint64_t, const DiscoveryRoute &, GameRoute *) {
    assert(false); return 1;
}

namespace ldn { struct ScanFilter; }
[[maybe_unused]] static int FindDirectNodeGroups(const DiscoveryRoute &, const ldn::ScanFilter *,
                                               void (*)(const char *, const void *, void *), void *) {
    assert(false); return -1;
}
