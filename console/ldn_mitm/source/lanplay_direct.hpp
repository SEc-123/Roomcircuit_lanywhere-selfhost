/* Direct LAN Play IPv4 transport. GPL-2.0-only. */
#pragma once
#include "../../../common/node_lobby.h"
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace ams::mitm::lanplay {
struct Endpoint {
    uint32_t ipv4;
    uint16_t port;
}; // network byte order
struct Snapshot {
    bool active = false, hosting = false;
    uint32_t node = 0, address = 0, sent = 0, received = 0;
    uint64_t epoch = 0, game = 0;
    uint8_t member = 0, members = 0;
    char error[64]{};
};
struct HostedRoom {
    uint64_t epoch = 0, game = 0;
    uint32_t node = 0, host_ip = 0;
    uint8_t network_id[32]{}, bssid[6]{};
    uint8_t flavors = 0; // Bit 1 << NodeFlavor for each currently available protocol.
};
using Found = void (*)(const char *, const void *, void *);
using StreamReply = void (*)(uint64_t, const void *, size_t);

bool Probe(const Endpoint &endpoint, unsigned timeout_ms = 1500);
bool Configure(uint32_t node, const Endpoint &endpoint);
int Scan(const void *filter, size_t filter_size, const NodeRoomLocator *selected, Found found, void *user);
bool Host(uint64_t game);
bool Join(const char *code, uint64_t game);
void Leave();
Snapshot State();
bool GetHostedRoom(HostedRoom *out);
void Publish(const void *info, size_t size);
bool SendControl(uint8_t destination, uint8_t kind, const void *data, size_t size);
ssize_t TakeControl(uint8_t *from, uint8_t *kind, void *out, size_t capacity);
int MemberOfIp(uint32_t ip);
bool SendData(uint32_t destination, uint16_t source_port, uint16_t destination_port, const void *data,
              size_t size);
ssize_t ReceiveData(uint16_t port, void *out, size_t capacity, uint32_t *source, uint16_t *source_port,
                    bool peek, uint32_t peer_ip = 0, uint16_t peer_port = 0);
bool HasData(uint16_t port, uint32_t peer_ip = 0, uint16_t peer_port = 0);
bool SendStream(uint64_t epoch, const void *body, size_t size);
void SetStreamReply(StreamReply reply);
void Shutdown();
} // namespace ams::mitm::lanplay
