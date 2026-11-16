/* Direct client for the documented LAN Play wire protocol. GPL-2.0-only. */
#include "lanplay_direct.hpp"
#if defined(__SWITCH__)
#include "storage_migration.hpp"
#include "../../../common/roomcircuit_paths.h"
#endif
#include "lanplay_codec.hpp"
#include "tcp_stream_protocol.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__SWITCH__)
#include <stratosphere.hpp>
#else
#include <thread>
#endif
extern "C" {
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
}

namespace {
uint64_t monotonic_ms() {
#if defined(__SWITCH__)
    return ams::os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}
} // namespace
extern "C" uint32_t sys_now(void) {
    return static_cast<uint32_t>(monotonic_ms());
}
extern "C" uint32_t lanplay_random(void) {
    uint32_t value = 0;
#if defined(__SWITCH__)
    ams::os::GenerateRandomBytes(&value, sizeof(value));
#else
    FILE *file = std::fopen("/dev/urandom", "rb");
    if (file) {
        if (std::fread(&value, 1, sizeof(value), file) != sizeof(value))
            value = 0;
        std::fclose(file);
    }
#endif
    return value ? value : static_cast<uint32_t>(monotonic_ms()) ^ 0x519758ab;
}

namespace ams::mitm::lanplay {
namespace {
constexpr uint32_t Network = 0x0a0d0000u, Broadcast = 0x0a0dffffu;
constexpr size_t InfoBytes = 0x480, NodeBytes = 64, FrameBytes = 0x600, GameBytes = 65507;
constexpr uint16_t LdnPort = 11452, ForkStreamPort = 11453, ForkControl = 0x4243, ForkGame = 0x474e;
constexpr size_t MaxFlows = 16, MaxScans = 32, StreamBuffer = 16384, StreamCredit = 8192;
bool Virtual(uint32_t ip) {
    return (ip & 0xffff0000u) == Network && (ip & 0xffffu) && (ip & 0xffffu) != 0xffffu && ip != 0x0a0d2501 &&
           ip != 0x0a0dfefe;
}
ip_addr_t Address(uint32_t host) {
    ip_addr_t result{};
    IP_ADDR4(&result, host >> 24, (host >> 16) & 255, (host >> 8) & 255, host & 255);
    return result;
}

struct Ring {
    std::array<uint8_t, StreamBuffer> data{};
    size_t start = 0, bytes = 0;
    size_t Free() const {
        return data.size() - bytes;
    }
    bool Push(const void *source, size_t size) {
        if (size > Free())
            return false;
        const auto *p = static_cast<const uint8_t *>(source);
        for (size_t i = 0; i < size; i++)
            data[(start + bytes + i) % data.size()] = p[i];
        bytes += size;
        return true;
    }
    size_t Peek(void *out, size_t size) const {
        size = std::min(size, bytes);
        auto *p = static_cast<uint8_t *>(out);
        for (size_t i = 0; i < size; i++)
            p[i] = data[(start + i) % data.size()];
        return size;
    }
    void Drop(size_t size) {
        size = std::min(size, bytes);
        start = (start + size) % data.size();
        bytes -= size;
    }
    void Clear() {
        start = bytes = 0;
    }
};
struct Control {
    uint8_t from = 0, kind = 0;
    uint16_t size = 0;
    uint8_t bytes[0x500]{};
};
struct FragmentPacket {
    uint16_t size = 0;
    uint8_t data[1201]{};
};
struct Datagram {
    uint32_t source = 0;
    uint16_t source_port = 0, destination_port = 0;
    size_t size = 0;
    uint8_t *data = nullptr;
};
struct Peer {
    uint32_t address = 0, real = 0;
    bool fork = false, admitted = false;
    uint64_t last = 0;
};
struct ScanEntry {
    uint8_t info[InfoBytes]{};
    NodeRoomLocator locator{};
    char code[8]{};
};
struct Assembly {
    uint32_t source = 0, destination = 0;
    uint16_t id = 0, pmtu = 0;
    uint8_t count = 0;
    uint32_t mask = 0;
    size_t bytes = 0;
    uint64_t at = 0;
    uint8_t data[8192]{};
};
uint32_t VirtualAddress(bool renew = false);
struct Impl;
struct Flow {
    Impl *owner = nullptr;
    tcp_pcb *pcb = nullptr;
    bool used = false, ldn = false, origin = false, fork = false, connected = false, local_fin = false,
         remote_fin = false, fin_sent = false, fin_reported = false;
    uint32_t peer = 0, id = 0;
    uint64_t nonce = 0, tx_received = 0, tx_acked = 0, rx_sent = 0, rx_acked = 0;
    uint16_t source_port = 0, destination_port = 0, window = 0, fork_id = 0;
    uint8_t member = 0;
    uint64_t began = 0;
    Ring incoming, outgoing;
    uint8_t ldn_frame[FrameBytes]{};
};
struct Listener {
    tcp_pcb *pcb = nullptr;
    uint16_t port = 0;
};
struct ForkMapping {
    uint8_t bssid[6]{};
    uint32_t real = 0, address = 0;
    uint64_t at = 0;
};
struct StreamEventPacket {
    uint64_t epoch = 0;
    size_t size = 0;
    uint8_t bytes[1100]{};
};

struct Impl {
    std::mutex mutex;
    std::atomic<bool> running{false};
    bool started = false, configured = false, active = false, hosting = false, joined = false,
         collision = false, renew_address = false;
    uint32_t node = 0, self = 0, sent = 0, received = 0, next_id = 1;
    uint64_t epoch = 0, game = 0, last_ping = 0, last_pong = 0, last_beacon = 0;
    uint16_t relay_fragment_id = 0, wire_id = 0, sequence = 0;
    uint8_t own_member = 0, host_flavor = NodeFlavorNone;
    uint8_t pending_join[NodeBytes + 6]{};
    uint64_t join_retry_at = 0;
    unsigned join_attempts = 0;
    Endpoint endpoint{};
    int socket = -1;
    netif interface{};
    raw_pcb *udp_raw = nullptr;
    uint8_t network_info[InfoBytes]{};
    bool published = false;
    char error[64]{};
    std::array<Peer, 8> peers{};
    std::array<Flow, MaxFlows> flows{};
    std::array<Listener, 10> listeners{};
    std::array<ScanEntry, MaxScans> scans{};
    size_t scan_count = 0;
    int selected_scan = -1;
    uint32_t scan_nonce = 0;
    bool scanning = false;
    uint8_t scan_filter[96]{};
    bool scan_has_filter = false, scan_has_selection = false;
    NodeRoomLocator scan_selection{};
    std::array<ForkMapping, 64> fork_mappings{};
    std::array<Control, 24> controls{};
    size_t control_head = 0, control_count = 0;
    std::array<Datagram, 24> packets{};
    size_t packet_head = 0, packet_count = 0, packet_bytes = 0;
    std::array<Assembly, 8> assemblies{};
    std::array<FragmentPacket, 128> outgoing_fragments{};
    size_t fragment_head = 0, fragment_count = 0;
    StreamReply stream_reply = nullptr;
    std::array<StreamEventPacket, 128> stream_events{};
    size_t event_head = 0, event_count = 0;
#if defined(__SWITCH__)
    ams::os::ThreadType thread{};
    alignas(ams::os::ThreadStackAlignment) uint8_t stack[0x10000]{};
    static void ThreadEntry(void *arg) {
        static_cast<Impl *>(arg)->Loop();
    }
#else
    std::thread thread;
#endif
    Impl() {
        lwip_init();
        ip4_addr_t zero{};
        netif_add(&interface, &zero, &zero, &zero, this, InitNetif, ip4_input);
        netif_set_default(&interface);
        netif_set_up(&interface);
        netif_set_link_up(&interface);
        udp_raw = raw_new(IP_PROTO_UDP);
        if (udp_raw)
            raw_recv(udp_raw, UdpInput, this);
        running = true;
#if defined(__SWITCH__)
        // libstratosphere adds UserThreadPriorityOffset (28): user16 -> kernel44.
        if (R_SUCCEEDED(ams::os::CreateThread(&thread, ThreadEntry, this, stack, sizeof(stack), 16))) {
            started = true;
            ams::os::StartThread(&thread);
        } else
            running = false;
#else
        thread = std::thread([this] { Loop(); });
        started = true;
#endif
    }
    ~Impl() {
        running = false;
        if (started) {
#if defined(__SWITCH__)
            ams::os::WaitThread(&thread);
            ams::os::DestroyThread(&thread);
#else
            thread.join();
#endif
        }
        Reset();
        if (socket >= 0)
            ::close(socket);
        if (udp_raw)
            raw_remove(udp_raw);
        netif_remove(&interface);
    }
    static err_t InitNetif(netif *net) {
        net->name[0] = 'l';
        net->name[1] = 'p';
        net->output = Output;
        net->mtu = 1200;
        net->flags = NETIF_FLAG_BROADCAST;
        return ERR_OK;
    }
    static err_t Output(netif *net, pbuf *packet, const ip4_addr_t *) {
        auto *self = static_cast<Impl *>(net->state);
        if (!self->configured || packet->tot_len < 20)
            return ERR_IF;
        auto &data = self->output_buffer;
        if (pbuf_copy_partial(packet, data + 1, packet->tot_len, 0) != packet->tot_len)
            return ERR_BUF;
        if (self->wire_id) {
            PutBe16(data + 5, self->wire_id);
            data[11] = data[12] = 0;
            PutBe16(data + 11, Fold(Sum(data + 1, (data[1] & 15) * 4)));
        }
        return self->SendIp(data + 1, packet->tot_len) ? ERR_OK : ERR_IF;
    }
    bool SendRaw(const void *data, size_t size) {
        if (socket < 0)
            return false;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = endpoint.ipv4;
        to.sin_port = endpoint.port;
        const ssize_t count = ::sendto(socket, data, size, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
        return count == static_cast<ssize_t>(size);
    }
    bool SendIp(const uint8_t *packet, size_t size) {
        if (size < 20 || size > 65535)
            return false;
        if (size <= 1200) {
            if (packet[9] == IP_PROTO_UDP && (Be16(packet + 6) & 0x3fff)) {
                if (fragment_count == outgoing_fragments.size())
                    return false;
                auto &fragment =
                    outgoing_fragments[(fragment_head + fragment_count++) % outgoing_fragments.size()];
                fragment.size = size + 1;
                fragment.data[0] = 1;
                std::memcpy(fragment.data + 1, packet, size);
                return true;
            }
            uint8_t data[1201];
            data[0] = 1;
            std::memcpy(data + 1, packet, size);
            return SendRaw(data, size + 1);
        }
        constexpr size_t part_bytes = 1024;
        const size_t count = (size + part_bytes - 1) / part_bytes;
        if (count > 8)
            return false;
        const uint16_t id = ++relay_fragment_id;
        for (size_t part = 0; part < count; part++) {
            uint8_t data[17 + part_bytes]{};
            data[0] = 3;
            std::memcpy(data + 1, packet + 12, 8);
            PutBe16(data + 9, id);
            data[11] = part;
            data[12] = count;
            const size_t bytes = std::min(part_bytes, size - part * part_bytes);
            PutBe16(data + 13, bytes);
            PutBe16(data + 15, part_bytes);
            std::memcpy(data + 17, packet + part * part_bytes, bytes);
            if (!SendRaw(data, 17 + bytes))
                return false;
        }
        return true;
    }
    void FlushFragments() {
        if (!active) {
            fragment_head = fragment_count = 0;
            return;
        }
        // Rust public relays use a ten-packet try_send queue. Pace only IPv4
        // UDP fragments; control packets and TCP retain their own scheduling.
        for (unsigned budget = 0; budget < 4 && fragment_count; budget++) {
            auto &fragment = outgoing_fragments[fragment_head];
            if (!SendRaw(fragment.data, fragment.size))
                break;
            fragment_head = (fragment_head + 1) % outgoing_fragments.size();
            fragment_count--;
        }
    }
    bool Udp(uint32_t destination, uint16_t source_port, uint16_t destination_port, const void *data,
             size_t size, uint16_t marker = 0) {
        if (size > GameBytes || (size && !data))
            return false;
        if (!marker && size + 28 > interface.mtu) {
            const size_t payload = (interface.mtu - 20) & ~size_t(7);
            const size_t fragments = (size + 8 + payload - 1) / payload;
            if (fragments > outgoing_fragments.size() - fragment_count)
                return false;
        }
        pbuf *packet = pbuf_alloc(PBUF_IP, static_cast<u16_t>(size + 8), PBUF_RAM);
        if (!packet)
            return false;
        auto *bytes = static_cast<uint8_t *>(packet->payload);
        PutBe16(bytes, source_port);
        PutBe16(bytes + 2, destination_port);
        PutBe16(bytes + 4, size + 8);
        bytes[6] = bytes[7] = 0;
        if (size)
            std::memcpy(bytes + 8, data, size);
        PutBe16(bytes + 6, UdpChecksum(self, destination, bytes, size + 8));
        ip4_addr_t source{htonl(self)}, target{htonl(destination)};
        // Fork magic IDs must remain intact; outer relay fragmentation handles their MTU.
        const auto mtu = interface.mtu;
        if (marker)
            interface.mtu = 65535;
        wire_id = marker;
        const err_t result = ip4_output_if(packet, &source, &target, 64, 0, IP_PROTO_UDP, &interface);
        wire_id = 0;
        interface.mtu = mtu;
        pbuf_free(packet);
        return result == ERR_OK;
    }
    bool LdnUdp(uint32_t destination, uint8_t kind, const void *data, size_t size, bool fork) {
        uint8_t frame[FrameBytes];
        const size_t bytes = EncodeLdn(frame, sizeof(frame), kind, data, size, fork ? ++sequence : 0);
        return bytes && Udp(destination, LdnPort, LdnPort, frame, bytes, fork ? ForkControl : 0);
    }
    int PeerFor(uint32_t address) const {
        for (unsigned i = 0; i < peers.size(); i++)
            if (peers[i].address == address)
                return i;
        return -1;
    }
    int AddPeer(uint32_t address, uint32_t real, bool fork) {
        if (!Virtual(address) || address == self || !real)
            return -1;
        const int existing = PeerFor(address);
        if (existing >= 0) {
            auto &peer = peers[existing];
            return peer.real == real && peer.fork == fork ? existing : -1;
        }
        for (unsigned i = hosting ? 1 : 0; i < peers.size(); i++)
            if (!peers[i].address) {
                peers[i] = {address, real, fork, false, monotonic_ms()};
                return i;
            }
        return -1;
    }
    void Queue(uint8_t from, uint8_t kind, const void *data, size_t size) {
        if (size > 0x500 || control_count == controls.size())
            return;
        auto &entry = controls[(control_head + control_count++) % controls.size()];
        entry.from = from;
        entry.kind = kind;
        entry.size = size;
        if (size)
            std::memcpy(entry.bytes, data, size);
    }
    void Data(uint32_t source, uint16_t sport, uint16_t dport, const void *data, size_t size) {
        if (packet_count == packets.size() || size > GameBytes || packet_bytes + size > 128 * 1024)
            return;
        auto *copy = static_cast<uint8_t *>(std::malloc(size ? size : 1));
        if (!copy)
            return;
        if (size)
            std::memcpy(copy, data, size);
        packets[(packet_head + packet_count++) % packets.size()] = {source, sport, dport, size, copy};
        packet_bytes += size;
        received++;
    }
    static u8_t UdpInput(void *context, raw_pcb *, pbuf *packet, const ip_addr_t *) {
        auto *self = static_cast<Impl *>(context);
        auto &bytes = self->ip_buffer;
        if (pbuf_copy_partial(packet, bytes, packet->tot_len, 0) != packet->tot_len)
            return 0;
        const size_t header = (bytes[0] & 15) * 4, total = packet->tot_len;
        if (header < 20 || total < header + 8)
            return 0;
        const uint32_t source = Be32(bytes + 12), destination = Be32(bytes + 16);
        const auto *udp = bytes + header;
        const size_t udp_size = Be16(udp + 4);
        if (udp_size < 8 || udp_size != total - header ||
            (Be16(udp + 6) && UdpChecksum(source, destination, udp, udp_size) != 0))
            return 0;
        self->OnUdp(source, destination, Be16(udp), Be16(udp + 2), udp + 8, udp_size - 8, Be16(bytes + 4));
        pbuf_free(packet);
        return 1;
    }
    bool MatchesScan(const uint8_t *info, uint32_t source, uint8_t flavor) const {
        if (scan_has_selection && !SameLocator(scan_selection, info, source, flavor))
            return false;
        if (!scan_has_filter)
            return true;
        const uint32_t flags = Le32(scan_filter + 92);
        if ((flags & 1) && std::memcmp(scan_filter, info, 8))
            return false;
        if ((flags & 2) && std::memcmp(scan_filter + 16, info + 16, 16))
            return false;
        if ((flags & 4) && !(Le32(scan_filter + 32) & info[0x4b]))
            return false;
        if ((flags & 8) && std::memcmp(scan_filter + 36, info + 0x20, 6))
            return false;
        if ((flags & 32) && Le16(scan_filter + 10) != Le16(info + 10))
            return false;
        return !(flags & 16) || (scan_filter[42] <= 32 && scan_filter[42] == info[0x26] &&
                                 !std::memcmp(scan_filter + 43, info + 0x27, scan_filter[42]));
    }
    void RememberMapping(const uint8_t *bssid, uint32_t real, uint32_t address) {
        if (!real || !Virtual(address))
            return;
        ForkMapping *slot = nullptr;
        const uint64_t now = monotonic_ms();
        for (auto &mapping : fork_mappings) {
            if (mapping.real && now - mapping.at > 10000)
                mapping = {};
            if (mapping.real && !std::memcmp(mapping.bssid, bssid, 6)) {
                if (mapping.real == real) {
                    if (mapping.address == address)
                        mapping.at = now;
                    return;
                }
                if (mapping.address == address)
                    return;
            }
            if (!mapping.real && !slot)
                slot = &mapping;
        }
        if (slot) {
            std::memcpy(slot->bssid, bssid, 6);
            slot->real = real;
            slot->address = address;
            slot->at = now;
        }
    }
    uint32_t Mapping(const uint8_t *bssid, uint32_t real) const {
        if (real == self)
            return self;
        for (const auto &mapping : fork_mappings)
            if (mapping.real == real && !std::memcmp(mapping.bssid, bssid, 6) &&
                monotonic_ms() - mapping.at <= 10000)
                return mapping.address;
        return Virtual(real) ? real : 0;
    }
    bool ScanInfo(ScanEntry &entry) const {
        if (entry.locator.flavor != NodeFlavorFork)
            return Le32(entry.info + 0x68) == entry.locator.host_ip;
        for (unsigned i = 0; i < 8; i++) {
            auto *node = entry.info + 0x68 + 64 * i;
            if (!node[11])
                continue;
            const uint32_t address = i ? Mapping(entry.locator.bssid, Le32(node)) : entry.locator.host_ip;
            if (!address)
                return false;
            Put32(node, address);
        }
        return ValidInfo(entry.info);
    }
    void RememberScan(uint32_t source, const uint8_t *info, uint8_t flavor) {
        if (!scanning || !ValidInfo(info) || !Virtual(source) || !MatchesScan(info, source, flavor))
            return;
        if (flavor == NodeFlavorFork)
            RememberMapping(info + 0x20, Le32(info + 0x68), source);
        for (size_t i = 0; i < scan_count; i++) {
            if (scans[i].locator.host_ip == source && scans[i].locator.flavor == flavor &&
                !std::memcmp(scans[i].info, info, 32) && !std::memcmp(scans[i].info + 0x20, info + 0x20, 6)) {
                std::memcpy(scans[i].info, info, InfoBytes);
                return;
            }
        }
        if (scan_count == scans.size())
            return;
        auto &entry = scans[scan_count++];
        std::memcpy(entry.info, info, InfoBytes);
        entry.locator = {};
        entry.locator.host_ip = source;
        entry.locator.flavor = flavor;
        std::memcpy(entry.locator.network_id, info, 32);
        std::memcpy(entry.locator.bssid, info + 0x20, 6);
        static constexpr char Alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint32_t hash = scan_nonce;
        for (unsigned i = 0; i < 5; i++) {
            entry.code[i] = Alphabet[hash % 32];
            hash >>= 5;
        }
        entry.code[5] = Alphabet[scan_count - 1];
        entry.code[6] = 0;
        std::memcpy(entry.locator.code, entry.code, 7);
    }
    bool Normalize(uint8_t *info, uint32_t host) {
        if (!ValidInfo(info))
            return false;
        for (unsigned i = 0; i < 8; i++) {
            uint8_t *n = info + 0x68 + 64 * i;
            if (!n[11])
                continue;
            const uint32_t real = Le32(n);
            uint32_t address = real;
            if (real != self) {
                address = 0;
                for (const auto &peer : peers)
                    if (peer.address && peer.real == real)
                        address = peer.address;
                if (!address && Virtual(real))
                    address = real;
                if (!address)
                    return false;
            }
            Put32(n, address);
        }
        return Le32(info + 0x68) == host && ValidInfo(info);
    }
    bool HostAllows(bool fork) const {
        return host_flavor == NodeFlavorNone || host_flavor == (fork ? NodeFlavorFork : NodeFlavorStandard);
    }
    void PinHostFlavor() {
        if (!hosting)
            return;
        if (host_flavor == NodeFlavorNone) {
            for (const auto &peer : peers) {
                if (peer.address && peer.address != self && peer.admitted) {
                    host_flavor = peer.fork ? NodeFlavorFork : NodeFlavorStandard;
                    break;
                }
            }
        }
        for (unsigned i = 1; i < peers.size(); i++) {
            if (peers[i].address && !HostAllows(peers[i].fork))
                DropPeer(i);
        }
    }
    void ForkInfo(uint8_t *info) const {
        std::memcpy(info, network_info, InfoBytes);
        for (unsigned i = 0; i < 8; i++) {
            auto *node_info = info + 0x68 + 64 * i;
            const int member = PeerFor(Le32(node_info));
            if (node_info[11] && member >= 0)
                Put32(node_info, peers[member].real);
        }
    }
    void SyncPeers(const uint8_t *info) {
        bool self_present = false;
        for (auto &peer : peers)
            peer.admitted = false;
        for (unsigned i = 0; i < 8; i++) {
            const auto *n = info + 0x68 + 64 * i;
            if (!n[11])
                continue;
            const uint32_t address = Le32(n);
            if (address == self) {
                own_member = i;
                self_present = true;
                continue;
            }
            int index = PeerFor(address);
            if (index < 0)
                index = AddPeer(address, address, false);
            if (index >= 0)
                peers[index].admitted = true;
        }
        joined = self_present;
    }
    void AddressCollision() {
        renew_address = true;
        if (active) {
            collision = true;
            std::snprintf(error, sizeof(error), "LAN Play address conflict. Reopen the room.");
            for (auto &flow : flows)
                ReleaseFlow(flow, false);
            active = joined = false;
            Queue(hosting ? 1 : 0, 0xff, nullptr, 0);
        } else {
            self = VirtualAddress(true);
            ip4_addr_t address{htonl(self)}, mask{htonl(0xffff0000)}, gateway{};
            netif_set_addr(&interface, &address, &mask, &gateway);
            renew_address = collision = false;
            last_ping = 0;
        }
    }
    void OnUdp(uint32_t source, uint32_t destination, uint16_t sport, uint16_t dport, const uint8_t *data,
               size_t size, uint16_t marker) {
        if (source == self) {
            AddressCollision();
            return;
        }
        if (!Virtual(source) ||
            (destination != self && destination != Broadcast && destination != UINT32_MAX))
            return;
        if (sport == LdnPort && dport == LdnPort) {
            uint8_t kind, decoded[0x500];
            size_t bytes = sizeof(decoded);
            uint16_t seq;
            if (!DecodeLdn(data, size, &kind, decoded, &bytes, &seq))
                return;
            const bool fork = marker == ForkControl;
            if (kind == 0 && hosting && published && network_info[0x62] != 1 && HostAllows(fork)) {
                uint8_t info[InfoBytes];
                if (fork)
                    ForkInfo(info);
                else
                    std::memcpy(info, network_info, InfoBytes);
                LdnUdp(fork ? Broadcast : source, 1, info, InfoBytes, fork);
            } else if (kind == 1 && bytes == InfoBytes)
                RememberScan(source, decoded, fork ? NodeFlavorFork : NodeFlavorStandard);
            else if (fork && kind == 2 && hosting && published && HostAllows(true) &&
                     bytes == NodeBytes + 6 && !std::memcmp(decoded + NodeBytes, network_info + 0x20, 6)) {
                const int member = AddPeer(source, Le32(decoded), true);
                if (member < 0 || decoded[11] != 1)
                    return;
                Put32(decoded, source);
                peers[member].last = monotonic_ms();
                Queue(member, 3, decoded, NodeBytes);
            } else if (fork && kind == 3 && !hosting && active && bytes == InfoBytes &&
                       source == peers[0].address && !std::memcmp(decoded, network_info, 32) &&
                       !std::memcmp(decoded + 0x20, network_info + 0x20, 6)) {
                if (Normalize(decoded, source)) {
                    std::memcpy(network_info, decoded, InfoBytes);
                    SyncPeers(decoded);
                    Queue(0, 4, decoded, InfoBytes);
                }
            } else if (fork && kind == 4 && bytes == 12) {
                RememberMapping(decoded, Le32(decoded + 8), source);
                if (active && !std::memcmp(decoded, network_info + 0x20, 6)) {
                    const int member = AddPeer(source, Le32(decoded + 8), true);
                    if (member >= 0)
                        peers[member].last = monotonic_ms();
                }
            } else if (fork && kind == 5 && active && bytes == 12 &&
                       !std::memcmp(decoded, network_info + 0x20, 6)) {
                const int member = PeerFor(source);
                if (member >= 0 && peers[member].real == Le32(decoded + 8))
                    DropPeer(member);
            }
            return;
        }
        const int member = PeerFor(source);
        if (!active || !joined || member < 0 || !peers[member].admitted)
            return;
        auto &peer = peers[member];
        peer.last = monotonic_ms();
        if (peer.fork) {
            if (marker == ForkGame) {
                if (size < 8 || Be32(data) != peer.real ||
                    (Be32(data + 4) != self && Be32(data + 4) != UINT32_MAX))
                    return;
                data += 8;
                size -= 8;
            } else if (marker != 0x474d)
                return;
            if (size > 1472)
                return;
            if (dport == ForkStreamPort) {
                ForkStream(source, data, size);
                return;
            }
        }
        Data(source, sport, dport, data, size);
    }
    Flow *AllocateFlow(uint32_t peer, bool ldn, bool origin) {
        for (auto &flow : flows)
            if (!flow.used) {
                flow = Flow{};
                flow.owner = this;
                flow.used = true;
                flow.peer = peer;
                flow.ldn = ldn;
                flow.origin = origin;
                flow.began = monotonic_ms();
                return &flow;
            }
        return nullptr;
    }
    void StreamEvent(Flow &flow, tcpstream::Op operation, uint16_t aux = 0, const uint8_t *data = nullptr,
                     size_t size = 0, uint64_t seq = 0) {
        if (!stream_reply || !flow.used)
            return;
        tcpstream::Frame frame{};
        frame.peer = flow.peer;
        frame.id = flow.id;
        frame.nonce = flow.nonce;
        frame.owner = flow.origin ? 1 : 0;
        frame.op = operation;
        frame.aux = aux;
        frame.data = data;
        frame.size = size;
        frame.seq = seq;
        if (operation == tcpstream::Op::Open) {
            frame.source_port = flow.source_port;
            frame.destination_port = flow.destination_port;
        }
        uint8_t bytes[tcpstream::PrefixBytes + tcpstream::HeaderBytes + tcpstream::MaxChunk];
        const size_t length = tcpstream::Encode(bytes, sizeof(bytes), frame);
        if (length) {
            if (event_count == stream_events.size()) {
                active = false;
                joined = false;
                std::snprintf(error, sizeof(error), "LAN Play stream queue full. Rejoin the room.");
                Queue(hosting ? 1 : 0, 0xff, nullptr, 0);
                return;
            }
            auto &event = stream_events[(event_head + event_count++) % stream_events.size()];
            event.epoch = epoch;
            event.size = length;
            std::memcpy(event.bytes, bytes, length);
        }
    }
    void ReleaseFlow(Flow &flow, bool notify = true) {
        if (!flow.used)
            return;
        if (notify && !flow.ldn)
            StreamEvent(flow, tcpstream::Op::Reset, static_cast<uint16_t>(tcpstream::Reason::Closed));
        const int member = flow.member;
        const bool was_ldn = flow.ldn;
        tcp_pcb *pcb = flow.pcb;
        flow.used = false;
        flow.pcb = nullptr;
        if (pcb) {
            tcp_arg(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_sent(pcb, nullptr);
            tcp_err(pcb, nullptr);
            tcp_abort(pcb);
        }
        if (was_ldn && notify && member < 8)
            DropPeer(member);
    }
    void DropPeer(unsigned member) {
        if (member >= peers.size() || !peers[member].address)
            return;
        const uint32_t address = peers[member].address;
        peers[member] = {};
        Queue(member, 0xff, nullptr, 0);
        for (auto &flow : flows)
            if (flow.used && flow.peer == address)
                ReleaseFlow(flow, false);
        if (!hosting && member == 0) {
            active = false;
            joined = false;
            std::snprintf(error, sizeof(error), "Game host disconnected. Rejoin the room.");
        }
    }
    static void TcpError(void *arg, err_t) {
        auto *flow = static_cast<Flow *>(arg);
        if (!flow || !flow->used)
            return;
        flow->pcb = nullptr;
        flow->owner->ReleaseFlow(*flow);
    }
    static err_t TcpSent(void *arg, tcp_pcb *, u16_t bytes) {
        auto *flow = static_cast<Flow *>(arg);
        if (!flow || !flow->used)
            return ERR_OK;
        if (!flow->ldn) {
            flow->tx_acked += bytes;
            flow->owner->StreamEvent(*flow, tcpstream::Op::Window, StreamCredit, nullptr, 0, flow->tx_acked);
        }
        flow->owner->Flush(*flow);
        return ERR_OK;
    }
    static err_t TcpReceive(void *arg, tcp_pcb *pcb, pbuf *packet, err_t error) {
        auto *flow = static_cast<Flow *>(arg);
        if (!flow || !flow->used) {
            if (packet)
                pbuf_free(packet);
            return ERR_OK;
        }
        auto &self = *flow->owner;
        if (error != ERR_OK) {
            if (packet)
                pbuf_free(packet);
            self.ReleaseFlow(*flow);
            return ERR_ABRT;
        }
        if (!packet) {
            flow->remote_fin = true;
            if (flow->ldn) {
                self.ReleaseFlow(*flow);
                return ERR_ABRT;
            }
            self.Flush(*flow);
            return ERR_OK;
        }
        if (flow->ldn) {
            if (packet->tot_len > flow->incoming.Free())
                return ERR_MEM;
            for (pbuf *part = packet; part; part = part->next)
                flow->incoming.Push(part->payload, part->len);
            tcp_recved(pcb, packet->tot_len);
            pbuf_free(packet);
            // TCP may split one LDN frame or coalesce several Sync messages.
            while (flow->used && flow->incoming.bytes >= 12) {
                flow->incoming.Peek(flow->ldn_frame, 12);
                const size_t length = 12 + Le16(flow->ldn_frame + 6);
                if (length > sizeof(flow->ldn_frame)) {
                    self.ReleaseFlow(*flow);
                    return ERR_ABRT;
                }
                if (length > flow->incoming.bytes)
                    break;
                flow->incoming.Peek(flow->ldn_frame, length);
                uint8_t kind, decoded[0x500];
                size_t size = sizeof(decoded);
                uint16_t seq;
                if (!DecodeLdn(flow->ldn_frame, length, &kind, decoded, &size, &seq)) {
                    self.ReleaseFlow(*flow);
                    return ERR_ABRT;
                }
                if (self.hosting && self.HostAllows(false) && kind == 2 && size == NodeBytes &&
                    Le32(decoded) == flow->peer && decoded[11] == 1) {
                    self.Queue(flow->member, 3, decoded, size);
                } else if (!self.hosting && kind == 3 && size == InfoBytes && ValidInfo(decoded) &&
                           Le32(decoded + 0x68) == flow->peer &&
                           !std::memcmp(decoded, self.network_info, 32) &&
                           !std::memcmp(decoded + 0x20, self.network_info + 0x20, 6)) {
                    std::memcpy(self.network_info, decoded, InfoBytes);
                    self.SyncPeers(decoded);
                    self.Queue(0, 4, decoded, size);
                } else {
                    self.ReleaseFlow(*flow);
                    return ERR_ABRT;
                }
                flow->incoming.Drop(length);
            }
            return flow->used ? ERR_OK : ERR_ABRT;
        }
        if (packet->tot_len > flow->incoming.Free())
            return ERR_MEM;
        for (pbuf *part = packet; part; part = part->next)
            flow->incoming.Push(part->payload, part->len);
        pbuf_free(packet);
        self.Flush(*flow);
        return ERR_OK;
    }
    static err_t TcpConnected(void *arg, tcp_pcb *, err_t error) {
        auto *flow = static_cast<Flow *>(arg);
        if (!flow || !flow->used)
            return ERR_ABRT;
        if (error != ERR_OK) {
            flow->owner->ReleaseFlow(*flow);
            return ERR_ABRT;
        }
        flow->connected = true;
        if (!flow->ldn)
            flow->owner->StreamEvent(*flow, tcpstream::Op::OpenOk, StreamCredit);
        flow->owner->Flush(*flow);
        return ERR_OK;
    }
    void Attach(Flow &flow, tcp_pcb *pcb) {
        flow.pcb = pcb;
        tcp_arg(pcb, &flow);
        tcp_recv(pcb, TcpReceive);
        tcp_sent(pcb, TcpSent);
        tcp_err(pcb, TcpError);
        tcp_nagle_disable(pcb);
    }
    static err_t TcpAccept(void *arg, tcp_pcb *pcb, err_t error) {
        auto &self = *static_cast<Impl *>(arg);
        if (error != ERR_OK)
            return error;
        const uint32_t address = ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip)));
        const bool ldn = pcb->local_port == LdnPort;
        int member = self.PeerFor(address);
        if (!self.active || !Virtual(address) ||
            (ldn ? (!self.hosting || !self.published || !self.HostAllows(false))
                 : (member < 0 || !self.joined || !self.peers[member].admitted))) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        if (ldn)
            member = self.AddPeer(address, address, false);
        auto *flow = member < 0 ? nullptr : self.AllocateFlow(address, ldn, false);
        if (!flow)
            return ERR_MEM;
        self.Attach(*flow, pcb);
        flow->member = member;
        flow->connected = ldn;
        flow->source_port = pcb->remote_port;
        flow->destination_port = pcb->local_port;
        if (!ldn) {
            flow->id = self.next_id++;
            flow->nonce = self.epoch;
            flow->window = StreamCredit;
            self.StreamEvent(*flow, tcpstream::Op::Open, StreamCredit, self.network_info, 32);
        }
        tcp_backlog_accepted(pcb);
        return ERR_OK;
    }
    bool Listen(uint16_t port) {
        for (auto &listener : listeners)
            if (listener.pcb && listener.port == port)
                return true;
        for (auto &listener : listeners)
            if (!listener.pcb) {
                tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
                if (!pcb)
                    return false;
                const ip_addr_t address = Address(self);
                if (tcp_bind(pcb, &address, port) != ERR_OK) {
                    tcp_abort(pcb);
                    return false;
                }
                err_t error;
                auto *listening = tcp_listen_with_backlog_and_err(pcb, 8, &error);
                if (!listening) {
                    tcp_abort(pcb);
                    return false;
                }
                listener = {listening, port};
                tcp_arg(listening, this);
                tcp_accept(listening, TcpAccept);
                return true;
            }
        // Listeners are created from admitted peers' SYNs. Reclaim an idle game
        // port when games rotate ephemeral ports; keep the control listener.
        for (auto &listener : listeners) {
            if (!listener.pcb || listener.port == LdnPort)
                continue;
            bool used = false;
            for (const auto &flow : flows)
                used |= flow.used && !flow.origin && flow.destination_port == listener.port;
            if (used)
                continue;
            tcp_arg(listener.pcb, nullptr);
            tcp_accept(listener.pcb, nullptr);
            tcp_close(listener.pcb);
            listener = {};
            return Listen(port);
        }
        return false;
    }
    Flow *Connect(uint32_t peer, uint16_t sport, uint16_t dport, bool ldn) {
        Flow *flow = AllocateFlow(peer, ldn, true);
        if (!flow)
            return nullptr;
        tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) {
            flow->used = false;
            return nullptr;
        }
        Attach(*flow, pcb);
        flow->source_port = sport;
        flow->destination_port = dport;
        const ip_addr_t local = Address(self), remote = Address(peer);
        if (tcp_bind(pcb, &local, sport) != ERR_OK ||
            tcp_connect(pcb, &remote, dport, TcpConnected) != ERR_OK) {
            ReleaseFlow(*flow, false);
            return nullptr;
        }
        flow->source_port = pcb->local_port;
        return flow;
    }
    void Flush(Flow &flow) {
        if (!flow.used || flow.fork || !flow.connected || !flow.pcb)
            return;
        uint8_t bytes[1024];
        while (flow.outgoing.bytes && tcp_sndbuf(flow.pcb)) {
            const size_t size =
                flow.outgoing.Peek(bytes, std::min<size_t>(sizeof(bytes), tcp_sndbuf(flow.pcb)));
            if (tcp_write(flow.pcb, bytes, size, TCP_WRITE_FLAG_COPY) != ERR_OK)
                break;
            flow.outgoing.Drop(size);
        }
        tcp_output(flow.pcb);
        if (flow.ldn)
            return;
        while (flow.incoming.bytes && flow.rx_sent - flow.rx_acked < flow.window) {
            const size_t available = flow.window - (flow.rx_sent - flow.rx_acked);
            const size_t bytes_count = flow.incoming.Peek(bytes, std::min(sizeof(bytes), available));
            StreamEvent(flow, tcpstream::Op::Data, 0, bytes, bytes_count, flow.rx_sent);
            flow.incoming.Drop(bytes_count);
            flow.rx_sent += bytes_count;
        }
        if (flow.local_fin && !flow.fin_sent && !flow.outgoing.bytes) {
            if (tcp_shutdown(flow.pcb, 0, 1) == ERR_OK)
                flow.fin_sent = true;
        }
        if (flow.remote_fin && !flow.fin_reported && !flow.incoming.bytes) {
            StreamEvent(flow, tcpstream::Op::Fin, 0, nullptr, 0, flow.rx_sent);
            flow.fin_reported = true;
        }
        if (flow.fin_sent && flow.fin_reported && flow.rx_acked == flow.rx_sent &&
            flow.tx_acked == flow.tx_received)
            ReleaseFlow(flow, false);
    }
    bool WriteLdn(Flow &flow, uint8_t kind, const void *data, size_t size) {
        uint8_t bytes[FrameBytes];
        const size_t length = EncodeLdn(bytes, sizeof(bytes), kind, data, size);
        if (!length || !flow.outgoing.Push(bytes, length))
            return false;
        Flush(flow);
        return true;
    }
    bool ForkData(uint32_t peer, uint16_t sport, uint16_t dport, const void *data, size_t size) {
        const int member = PeerFor(peer);
        if (member < 0 || size > 1472)
            return false;
        uint8_t bytes[1480];
        PutBe32(bytes, self);
        PutBe32(bytes + 4, peers[member].real);
        if (size)
            std::memcpy(bytes + 8, data, size);
        return Udp(peer, sport, dport, bytes, size + 8, ForkGame);
    }
    bool ForkWire(Flow &flow, uint8_t kind, uint16_t port, const void *payload, size_t size) {
        if (size > 1024)
            return false;
        uint8_t bytes[1032]{};
        bytes[0] = kind;
        bytes[1] = flow.origin ? 0 : 1;
        Put16(bytes + 2, flow.fork_id);
        Put16(bytes + 4, port);
        Put16(bytes + 6, size);
        if (size)
            std::memcpy(bytes + 8, payload, size);
        return ForkData(flow.peer, ForkStreamPort, ForkStreamPort, bytes, 8 + size);
    }
    void ForkStream(uint32_t peer, const uint8_t *bytes, size_t size) {
        if (size < 8 || bytes[1] > 1 || Le16(bytes + 6) != size - 8 || size - 8 > 1024)
            return;
        const uint8_t kind = bytes[0];
        const bool origin = bytes[1] == 1;
        const uint16_t id = Le16(bytes + 2), port = Le16(bytes + 4);
        Flow *flow = nullptr;
        for (auto &candidate : flows)
            if (candidate.used && candidate.fork && candidate.peer == peer && candidate.origin == origin &&
                candidate.fork_id == id)
                flow = &candidate;
        if (kind == 1) {
            if (origin || !port || size != 8)
                return;
            if (flow)
                return;
            flow = AllocateFlow(peer, false, false);
            if (!flow)
                return;
            flow->fork = true;
            flow->fork_id = id;
            flow->id = next_id++;
            flow->nonce = epoch;
            flow->source_port = static_cast<uint16_t>(49152 + (id % 16000));
            flow->destination_port = port;
            StreamEvent(*flow, tcpstream::Op::Open, StreamCredit, network_info, 32);
            return;
        }
        if (!flow || port)
            return;
        if (kind == 3 && size == 8) {
            ReleaseFlow(*flow);
            return;
        }
        if (kind != 2 || size == 8)
            return;
        if (!flow->incoming.Push(bytes + 8, size - 8)) {
            ForkWire(*flow, 3, 0, nullptr, 0);
            ReleaseFlow(*flow);
            return;
        }
        FlushFork(*flow);
    }
    void FlushFork(Flow &flow) {
        if (!flow.used || !flow.connected)
            return;
        uint8_t data[1024];
        while (flow.incoming.bytes && flow.rx_sent - flow.rx_acked < flow.window) {
            const size_t size = flow.incoming.Peek(
                data, std::min<uint64_t>(sizeof(data), flow.window - (flow.rx_sent - flow.rx_acked)));
            StreamEvent(flow, tcpstream::Op::Data, 0, data, size, flow.rx_sent);
            flow.rx_sent += size;
            flow.incoming.Drop(size);
        }
    }
    bool Stream(const tcpstream::Frame &frame) {
        if (!active || !joined ||
            std::memcmp(frame.data ? frame.data : network_info, network_info,
                        frame.op == tcpstream::Op::Open ? 32 : 0))
            return false;
        const int member = PeerFor(frame.peer);
        if (member < 0 || !peers[member].admitted)
            return false;
        Flow *flow = nullptr;
        for (auto &item : flows)
            if (item.used && !item.ldn && item.peer == frame.peer && item.id == frame.id &&
                item.nonce == frame.nonce && item.origin == (frame.owner == 0))
                flow = &item;
        if (frame.op == tcpstream::Op::Open) {
            if (flow)
                return false;
            if (peers[member].fork) {
                flow = AllocateFlow(frame.peer, false, true);
                if (!flow)
                    return false;
                flow->fork = true;
                flow->connected = true;
                flow->fork_id = next_id++ & 0xffff;
            } else
                flow = Connect(frame.peer, frame.source_port, frame.destination_port, false);
            if (!flow)
                return false;
            flow->id = frame.id;
            flow->nonce = frame.nonce;
            flow->member = member;
            flow->window = frame.aux;
            flow->source_port = frame.source_port;
            flow->destination_port = frame.destination_port;
            if (flow->fork) {
                if (!ForkWire(*flow, 1, frame.destination_port, nullptr, 0)) {
                    ReleaseFlow(*flow, false);
                    return false;
                }
                StreamEvent(*flow, tcpstream::Op::LegacyReady, StreamCredit);
            }
            return true;
        }
        if (!flow)
            return false;
        switch (frame.op) {
        case tcpstream::Op::OpenOk:
        case tcpstream::Op::LegacyReady:
            flow->connected = true;
            flow->window = frame.aux;
            break;
        case tcpstream::Op::Data:
            if (frame.seq != flow->tx_received ||
                frame.size > StreamCredit - (flow->tx_received - flow->tx_acked))
                return false;
            if (flow->fork) {
                if (!ForkWire(*flow, 2, 0, frame.data, frame.size))
                    return false;
                flow->tx_received += frame.size;
                flow->tx_acked = flow->tx_received;
                StreamEvent(*flow, tcpstream::Op::Window, StreamCredit, nullptr, 0, flow->tx_acked);
            } else {
                if (!flow->outgoing.Push(frame.data, frame.size))
                    return false;
                flow->tx_received += frame.size;
            }
            break;
        case tcpstream::Op::Window:
            if (frame.seq < flow->rx_acked || frame.seq > flow->rx_sent || frame.aux > tcpstream::MaxWindow)
                return false;
            if (flow->pcb)
                tcp_recved(flow->pcb, frame.seq - flow->rx_acked);
            flow->rx_acked = frame.seq;
            flow->window = frame.aux;
            break;
        case tcpstream::Op::Fin:
            if (frame.seq != flow->tx_received)
                return false;
            if (flow->fork) {
                ForkWire(*flow, 3, 0, nullptr, 0);
                ReleaseFlow(*flow, false);
                return true;
            }
            flow->local_fin = true;
            break;
        case tcpstream::Op::Reset:
            if (flow->fork)
                ForkWire(*flow, 3, 0, nullptr, 0);
            ReleaseFlow(*flow, false);
            return true;
        default:
            return false;
        }
        if (flow->fork)
            FlushFork(*flow);
        else
            Flush(*flow);
        return true;
    }
    void InputIp(const uint8_t *bytes, size_t size) {
        if (size < 20 || size > 65535 || (bytes[0] >> 4) != 4 || Be16(bytes + 2) != size)
            return;
        const size_t header = (bytes[0] & 15) * 4;
        if (header < 20 || header > size || Fold(Sum(bytes, header)))
            return;
        const uint32_t source = Be32(bytes + 12), destination = Be32(bytes + 16);
        if (source == self) {
            AddressCollision();
            return;
        }
        if (!Virtual(source) ||
            (destination != self && destination != Broadcast && destination != UINT32_MAX))
            return;
        if (bytes[9] == 6 && !(Be16(bytes + 6) & 0x3fff) && size >= header + 20 &&
            (bytes[header + 13] & 0x12) == 2) {
            const uint16_t port = Be16(bytes + header + 2);
            const int member = PeerFor(source);
            if (port != LdnPort && active && joined && member >= 0 && peers[member].admitted)
                Listen(port);
        }
        pbuf *packet = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (!packet)
            return;
        if (pbuf_take(packet, bytes, size) != ERR_OK) {
            pbuf_free(packet);
            return;
        }
        if (interface.input(packet, &interface) != ERR_OK)
            pbuf_free(packet);
    }
    void InputFragment(const uint8_t *bytes, size_t size) {
        if (size < 16)
            return;
        const bool little = Le16(bytes + 12) == size - 16;
        if (!little && Be16(bytes + 12) != size - 16)
            return;
        const uint16_t id = little ? Le16(bytes + 8) : Be16(bytes + 8),
                       part_bytes = little ? Le16(bytes + 14) : Be16(bytes + 14);
        const uint8_t part = bytes[10], count = bytes[11];
        if (!part_bytes || !count || count > 8 || part >= count || size == 16 || size - 16 > part_bytes ||
            size_t(part_bytes) * count > 8192 || (part + 1 < count && size - 16 != part_bytes))
            return;
        const uint32_t source = Be32(bytes), destination = Be32(bytes + 4);
        if (!Virtual(source) ||
            (destination != self && destination != Broadcast && destination != UINT32_MAX))
            return;
        Assembly *assembly = nullptr;
        for (auto &item : assemblies) {
            if (item.count && monotonic_ms() - item.at > 5000)
                item = {};
            if (item.count && item.source == source && item.destination == destination && item.id == id)
                assembly = &item;
        }
        if (!assembly)
            for (auto &item : assemblies)
                if (!item.count) {
                    assembly = &item;
                    item.source = source;
                    item.destination = destination;
                    item.id = id;
                    item.pmtu = part_bytes;
                    item.count = count;
                    item.at = monotonic_ms();
                    break;
                }
        if (!assembly)
            return;
        if (assembly->count != count || assembly->pmtu != part_bytes) {
            *assembly = {};
            return;
        }
        if (assembly->mask & (1u << part))
            return;
        std::memcpy(assembly->data + size_t(part) * part_bytes, bytes + 16, size - 16);
        assembly->mask |= 1u << part;
        assembly->bytes = std::max(assembly->bytes, size_t(part) * part_bytes + size - 16);
        if (assembly->mask == (1u << count) - 1) {
            InputIp(assembly->data, assembly->bytes);
            *assembly = {};
        }
    }
    void Reset() {
        for (auto &flow : flows)
            ReleaseFlow(flow, false);
        for (auto &listener : listeners)
            if (listener.pcb) {
                tcp_arg(listener.pcb, nullptr);
                tcp_accept(listener.pcb, nullptr);
                tcp_close(listener.pcb);
                listener = {};
            }
        for (auto &packet : packets) {
            std::free(packet.data);
            packet = {};
        }
        peers = {};
        assemblies = {};
        fragment_head = fragment_count = 0;
        active = hosting = joined = published = false;
        collision = false;
        packet_count = packet_head = packet_bytes = control_head = control_count = event_head = event_count =
            0;
        std::memset(network_info, 0, sizeof(network_info));
        game = 0;
        own_member = 0;
        host_flavor = NodeFlavorNone;
        join_retry_at = 0;
        join_attempts = 0;
        error[0] = 0;
        epoch++;
    }
    void KeepAlive() {
        const uint64_t now = monotonic_ms();
        if (now - last_ping >= 5000) {
            last_ping = now;
            uint8_t ping[5];
            ping[0] = 2;
            Put32(ping + 1, static_cast<uint32_t>(now));
            if (!SendRaw(ping, sizeof(ping)) || (last_pong && now - last_pong > 15000)) {
                const int replacement = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (replacement >= 0 && ::fcntl(replacement, F_SETFL, O_NONBLOCK) < 0) {
                    ::close(replacement);
                } else if (replacement >= 0) {
                    if (socket >= 0)
                        ::close(socket);
                    socket = replacement;
                }
            }
            // A tiny IPv4 scan refreshes the relay's source-to-endpoint mapping.
            LdnUdp(Broadcast, 0, nullptr, 0, false);
        }
        if (active && now - last_pong > 55000) {
            for (auto &flow : flows)
                ReleaseFlow(flow, false);
            active = joined = false;
            std::snprintf(error, sizeof(error), "LAN Play node disconnected. Rejoin the room.");
            Queue(hosting ? 1 : 0, 0xff, nullptr, 0);
        }
        if (active && now - last_beacon >= 2000) {
            last_beacon = now;
            if (hosting && published && HostAllows(true)) {
                uint8_t info[InfoBytes];
                ForkInfo(info);
                LdnUdp(Broadcast, 1, info, InfoBytes, true);
                for (const auto &peer : peers) {
                    if (peer.fork && peer.admitted) {
                        LdnUdp(Broadcast, 3, info, InfoBytes, true);
                        break;
                    }
                }
            }
            if (joined) {
                uint8_t beat[12]{};
                std::memcpy(beat, network_info + 0x20, 6);
                Put32(beat + 8, self);
                LdnUdp(Broadcast, 4, beat, sizeof(beat), true);
            }
            for (unsigned i = 0; i < peers.size(); i++)
                if (peers[i].address && peers[i].fork && now - peers[i].last > 15000)
                    DropPeer(i);
        }
        if (active && !hosting && !joined && join_attempts && now - join_retry_at >= 500) {
            if (join_attempts < 8) {
                LdnUdp(Broadcast, 2, pending_join, sizeof(pending_join), true);
                join_retry_at = now;
                join_attempts++;
            } else {
                active = false;
                std::snprintf(error, sizeof(error), "LAN Play room did not respond. Try again.");
                Queue(0, 0xff, nullptr, 0);
            }
        }
        for (auto &flow : flows)
            if (flow.used) {
                if (!flow.connected && now - flow.began > 5000)
                    ReleaseFlow(flow);
                else if (flow.fork)
                    FlushFork(flow);
                else
                    Flush(flow);
            }
        if (collision && active) {
            for (auto &flow : flows)
                ReleaseFlow(flow, false);
            active = false;
            joined = false;
            Queue(hosting ? 1 : 0, 0xff, nullptr, 0);
        }
    }
    void Loop() {
        while (running) {
            {
                std::scoped_lock lock(mutex);
                if (configured) {
                    for (unsigned budget = 0; budget < 32; budget++) {
                        sockaddr_in from{};
                        socklen_t from_size = sizeof(from);
                        const ssize_t size = ::recvfrom(socket, receive_buffer, sizeof(receive_buffer), 0,
                                                        reinterpret_cast<sockaddr *>(&from), &from_size);
                        if (size < 0)
                            break;
                        if (from.sin_addr.s_addr != endpoint.ipv4 || from.sin_port != endpoint.port ||
                            size < 1)
                            continue;
                        if (receive_buffer[0] == 2 && (size == 4 || size == 5)) {
                            uint8_t expected[5];
                            expected[0] = 2;
                            Put32(expected + 1, static_cast<uint32_t>(last_ping));
                            if (!std::memcmp(expected, receive_buffer, size))
                                last_pong = monotonic_ms();
                            continue;
                        }
                        if (receive_buffer[0] == 1)
                            InputIp(receive_buffer + 1, size - 1);
                        else if (receive_buffer[0] == 3)
                            InputFragment(receive_buffer + 1, size - 1);
                    }
                    sys_check_timeouts();
                    KeepAlive();
                    FlushFragments();
                }
            }
            for (unsigned budget = 0; budget < 128; budget++) {
                StreamEventPacket event;
                StreamReply callback;
                {
                    std::scoped_lock lock(mutex);
                    if (!event_count)
                        break;
                    event = stream_events[event_head];
                    event_head = (event_head + 1) % stream_events.size();
                    event_count--;
                    callback = stream_reply;
                }
                if (callback)
                    callback(event.epoch, event.bytes, event.size);
            }
#if defined(__SWITCH__)
            svcSleepThread(10000000L);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
        }
    }
    uint8_t receive_buffer[8193]{}, ip_buffer[65536]{}, output_buffer[65536]{};
};

Impl *instance = nullptr;
bool terminated = false;
std::mutex instance_mutex;
alignas(Impl) uint8_t instance_storage[sizeof(Impl)];
Impl *Get() {
    std::scoped_lock lock(instance_mutex);
    if (terminated)
        return nullptr;
    if (!instance)
        instance = new (instance_storage) Impl();
    return instance;
}
Impl *Peek() {
    std::scoped_lock lock(instance_mutex);
    return instance;
}
void Sleep(unsigned milliseconds) {
#if defined(__SWITCH__)
    svcSleepThread(static_cast<int64_t>(milliseconds) * 1000000);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
}
uint32_t VirtualAddress(bool renew) {
    uint8_t saved[8]{};
    uint32_t address = 0;
#if defined(__SWITCH__)
    // This sysmodule mounts sdmc through ams::fs, not libnx's stdio devoptab.
    namespace fs = ams::fs;
    const char *Path = rc_storage_path(ams::mitm::storage::StorageUseCurrent(), RC_LEGACY_PATH("lanplay-ip.bin"), RC_CURRENT_PATH("lanplay-ip.bin"));
    const char *TemporaryPath = rc_storage_path(ams::mitm::storage::StorageUseCurrent(), RC_LEGACY_PATH("lanplay-ip.bin.tmp"), RC_CURRENT_PATH("lanplay-ip.bin.tmp"));
    const char *PreviousPath = rc_storage_path(ams::mitm::storage::StorageUseCurrent(), RC_LEGACY_PATH("lanplay-ip.bin.previous"), RC_CURRENT_PATH("lanplay-ip.bin.previous"));
    auto read = [](const char *path, uint32_t *out) {
        fs::FileHandle file;
        if (R_FAILED(fs::OpenFile(&file, path, fs::OpenMode_Read)))
            return false;
        uint8_t record[8]{};
        size_t bytes = 0;
        int64_t size = 0;
        const bool valid = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size == static_cast<int64_t>(sizeof(record)) &&
                           R_SUCCEEDED(fs::ReadFile(&bytes, file, 0, record, sizeof(record))) &&
                           bytes == sizeof(record) && !std::memcmp(record, "LPIP", 4) &&
                           Virtual(Le32(record + 4));
        fs::CloseFile(file);
        if (valid)
            *out = Le32(record + 4);
        return valid;
    };
    const bool primary_valid = read(Path, &address);
    if (!primary_valid)
        read(PreviousPath, &address);
#endif
    if (Virtual(address) && !renew)
        return address;
    const uint32_t previous_address = address;
    do {
        address = Network | (lanplay_random() & 0xffff);
    } while (!Virtual(address) || (renew && address == previous_address));
#if defined(__SWITCH__)
    // Stage and verify before replacing the last valid identity. Any storage
    // failure keeps this session usable and leaves the old file recoverable.
    static_cast<void>(fs::CreateDirectory(ams::mitm::storage::StorageUseCurrent() ? RC_CURRENT_DIR : RC_LEGACY_DIR));
    std::memcpy(saved, "LPIP", 4);
    Put32(saved + 4, address);
    static_cast<void>(fs::DeleteFile(TemporaryPath));
    if (R_FAILED(fs::CreateFile(TemporaryPath, sizeof(saved))))
        return address;
    fs::FileHandle file;
    bool written = false;
    if (R_SUCCEEDED(fs::OpenFile(&file, TemporaryPath, fs::OpenMode_Write))) {
        written = R_SUCCEEDED(fs::WriteFile(file, 0, saved, sizeof(saved), fs::WriteOption::Flush));
        fs::CloseFile(file);
    }
    uint32_t verified = 0;
    written = written && read(TemporaryPath, &verified) && verified == address;
    if (written && primary_valid) {
        const auto deleted = fs::DeleteFile(PreviousPath);
        written = R_SUCCEEDED(deleted) || fs::ResultPathNotFound::Includes(deleted);
    }
    bool moved_previous = false;
    if (written && primary_valid) {
        const auto moved = fs::RenameFile(Path, PreviousPath);
        moved_previous = R_SUCCEEDED(moved);
        written = moved_previous;
    } else if (written) {
        // A recovered previous record may be the only valid copy; preserve it.
        const auto deleted = fs::DeleteFile(Path);
        written = R_SUCCEEDED(deleted) || fs::ResultPathNotFound::Includes(deleted);
    }
    if (!written || R_FAILED(fs::RenameFile(TemporaryPath, Path))) {
        if (moved_previous)
            static_cast<void>(fs::RenameFile(PreviousPath, Path));
        static_cast<void>(fs::DeleteFile(TemporaryPath));
    }
#else
    (void)saved;
#endif
    return address;
}
} // namespace

bool Probe(const Endpoint &endpoint, unsigned timeout_ms, selection_trace::Handle trace) {
    selection_trace::Record(trace, "probe.socket.begin");
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    selection_trace::Record(trace, "probe.socket.end", fd < 0 ? 1 : 0);
    if (fd < 0)
        return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = endpoint.ipv4;
    destination.sin_port = endpoint.port;
    uint8_t ping[5];
    ping[0] = 2;
    selection_trace::Record(trace, "probe.random.begin");
    Put32(ping + 1, lanplay_random());
    selection_trace::Record(trace, "probe.random.end");
    bool valid = false;
    selection_trace::Record(trace, "probe.send.begin");
    if (::sendto(fd, ping, sizeof(ping), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&destination),
                 sizeof(destination)) == sizeof(ping)) {
        selection_trace::Record(trace, "probe.send.end");
        const uint64_t deadline = monotonic_ms() + timeout_ms;
        bool received_once = false;
        int ready = 0;
        selection_trace::Record(trace, "probe.poll.begin");
        while (!valid) {
            const uint64_t now = monotonic_ms();
            if (now >= deadline) break;
            pollfd request{fd, POLLIN, 0};
            const int remaining = static_cast<int>(std::min<uint64_t>(deadline - now, 50));
            ready = ::poll(&request, 1, remaining);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (!(request.revents & POLLIN)) {
                if (request.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                continue;
            }
            uint8_t reply[8];
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            // Log the first receive boundary without SD writes for every stray
            // packet. Unrelated datagrams cannot consume a new timeout budget.
            if (!received_once) selection_trace::Record(trace, "probe.receive.begin");
            const ssize_t bytes =
                ::recvfrom(fd, reply, sizeof(reply), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &size);
            if (!received_once) {
                selection_trace::Record(trace, "probe.receive.end", static_cast<uint32_t>(bytes));
                received_once = true;
            }
            if (bytes < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            valid = (bytes == 4 || bytes == 5) && size >= sizeof(from) && from.sin_family == AF_INET &&
                    from.sin_addr.s_addr == endpoint.ipv4 && from.sin_port == endpoint.port &&
                    !std::memcmp(reply, ping, static_cast<size_t>(bytes));
        }
        selection_trace::Record(trace, "probe.poll.end", static_cast<uint32_t>(ready));
    }
    selection_trace::Record(trace, "probe.close.begin");
    ::close(fd);
    selection_trace::Record(trace, "probe.close.end", valid ? 0 : 1);
    return valid;
}
bool Configure(uint32_t node, const Endpoint &endpoint) {
    auto *impl = Get();
    if (!impl || !impl->started || !endpoint.ipv4 || !endpoint.port)
        return false;
    std::scoped_lock lock(impl->mutex);
    if (impl->scanning)
        return false;
    if (impl->active)
        return impl->node == node && impl->endpoint.ipv4 == endpoint.ipv4 &&
               impl->endpoint.port == endpoint.port;
    if (impl->configured && impl->socket >= 0 && !impl->renew_address && impl->node == node &&
        impl->endpoint.ipv4 == endpoint.ipv4 && impl->endpoint.port == endpoint.port)
        return true;
    impl->Reset();
    impl->configured = false;
    impl->fork_mappings = {};
    if (impl->socket >= 0)
        ::close(impl->socket);
    impl->socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl->socket < 0)
        return false;
    if (::fcntl(impl->socket, F_SETFL, O_NONBLOCK) < 0) {
        ::close(impl->socket);
        impl->socket = -1;
        return false;
    }
    impl->node = node;
    impl->endpoint = endpoint;
    impl->configured = true;
    impl->self = VirtualAddress(impl->renew_address);
    impl->renew_address = false;
    ip4_addr_t address{htonl(impl->self)}, mask{htonl(0xffff0000)}, gateway{};
    netif_set_addr(&impl->interface, &address, &mask, &gateway);
    impl->last_ping = 0;
    impl->last_pong = monotonic_ms();
    return true;
}
int Scan(const void *filter, size_t filter_size, const NodeRoomLocator *selected, Found found, void *user) {
    auto *impl = Get();
    if (!impl)
        return -1;
    {
        std::scoped_lock lock(impl->mutex);
        if (!impl->configured || impl->active || impl->scanning)
            return -1;
        if (filter && filter_size != sizeof(impl->scan_filter))
            return -1;
        impl->scan_has_filter = filter != nullptr;
        if (filter)
            std::memcpy(impl->scan_filter, filter, sizeof(impl->scan_filter));
        impl->scan_has_selection = selected != nullptr;
        if (selected)
            impl->scan_selection = *selected;
        impl->scanning = true;
        impl->scan_count = 0;
        impl->selected_scan = -1;
        impl->scan_nonce = lanplay_random();
        impl->LdnUdp(selected ? selected->host_ip : Broadcast, 0, nullptr, 0, false);
        impl->LdnUdp(selected ? selected->host_ip : Broadcast, 0, nullptr, 0, true);
        if (selected && selected->flavor == NodeFlavorFork)
            impl->LdnUdp(Broadcast, 0, nullptr, 0, true);
    }
    Sleep(900);
    std::unique_ptr<ScanEntry[], decltype(&std::free)> result(
        static_cast<ScanEntry *>(std::calloc(MaxScans, sizeof(ScanEntry))), &std::free);
    size_t count = 0;
    if (!result) {
        std::scoped_lock lock(impl->mutex);
        impl->scanning = false;
        return -1;
    }
    {
        std::scoped_lock lock(impl->mutex);
        impl->scanning = false;
        for (size_t i = 0; i < impl->scan_count; i++) {
            auto &entry = impl->scans[i];
            if (selected && !SameLocator(*selected, entry.info, entry.locator.host_ip, entry.locator.flavor))
                continue;
            if (selected && selected->code[0]) {
                std::memcpy(entry.code, selected->code, 8);
                impl->selected_scan = static_cast<int>(i);
            }
            result[count] = entry;
            if (impl->ScanInfo(result[count]))
                count++;
        }
    }
    int emitted = 0;
    // A dual-capable host can answer both scans. Prefer reliable standard TCP
    // for game discovery; an explicitly selected directory locator stays exact.
    for (uint8_t flavor : {NodeFlavorStandard, NodeFlavorFork}) {
        for (size_t i = 0; i < count; i++) {
            const auto &entry = result[i];
            if (entry.locator.flavor != flavor)
                continue;
            bool duplicate = false;
            if (!selected && flavor == NodeFlavorFork) {
                for (size_t j = 0; j < count; j++) {
                    const auto &candidate = result[j].locator;
                    duplicate |= candidate.flavor == NodeFlavorStandard &&
                                 candidate.host_ip == entry.locator.host_ip &&
                                 !std::memcmp(candidate.network_id, entry.locator.network_id, 32) &&
                                 !std::memcmp(candidate.bssid, entry.locator.bssid, 6);
                }
            }
            if (duplicate)
                continue;
            if (found)
                found(entry.code, entry.info, user);
            emitted++;
        }
    }
    return emitted;
}
bool Host(uint64_t game) {
    auto *impl = Get();
    if (!impl)
        return false;
    std::scoped_lock lock(impl->mutex);
    if (!impl->configured || impl->active || impl->scanning || impl->collision)
        return false;
    impl->Reset();
    impl->active = impl->hosting = impl->joined = true;
    impl->game = game;
    impl->own_member = 0;
    impl->peers[0] = {impl->self, impl->self, false, true, monotonic_ms()};
    impl->epoch = (uint64_t(lanplay_random()) << 32) | lanplay_random() | (uint64_t(1) << 63);
    if (!impl->epoch)
        impl->epoch = 1;
    if (!impl->Listen(LdnPort)) {
        impl->Reset();
        return false;
    }
    return true;
}
bool Join(const char *code, uint64_t game) {
    auto *impl = Get();
    if (!impl)
        return false;
    std::unique_lock lock(impl->mutex);
    if (!impl->configured || impl->active || impl->scanning || impl->collision)
        return false;
    ScanEntry selected{};
    bool found = false;
    for (size_t i = 0; i < impl->scan_count; i++)
        if ((impl->selected_scan < 0 || impl->selected_scan == static_cast<int>(i)) &&
            !std::memcmp(impl->scans[i].code, code, 6) &&
            (uint64_t(Le32(impl->scans[i].info)) | (uint64_t(Le32(impl->scans[i].info + 4)) << 32)) == game) {
            selected = impl->scans[i];
            found = true;
            break;
        }
    if (!found)
        return false;
    impl->Reset();
    impl->active = true;
    impl->game = game;
    impl->epoch = (uint64_t(lanplay_random()) << 32) | lanplay_random() | (uint64_t(1) << 63);
    if (!impl->epoch)
        impl->epoch = 1;
    std::memcpy(impl->network_info, selected.info, InfoBytes);
    const bool fork = selected.locator.flavor == NodeFlavorFork;
    const uint32_t host = selected.locator.host_ip;
    impl->peers[0] = {host, Le32(selected.info + 0x68), fork, true, monotonic_ms()};
    impl->own_member = 1;
    if (fork)
        for (unsigned i = 1; i < 8; i++) {
            const auto *member = selected.info + 0x68 + 64 * i;
            if (!member[11])
                continue;
            const uint32_t address = impl->Mapping(selected.locator.bssid, Le32(member));
            if (!address || impl->AddPeer(address, Le32(member), true) < 0) {
                impl->Reset();
                return false;
            }
        }
    if (fork)
        return true;
    Flow *flow = impl->Connect(host, 0, LdnPort, true);
    if (!flow) {
        impl->Reset();
        return false;
    }
    flow->member = 0;
    for (unsigned i = 0; i < 150; i++) {
        if (!flow->used || !impl->active)
            break;
        if (flow->connected)
            return true;
        lock.unlock();
        Sleep(20);
        lock.lock();
    }
    impl->Reset();
    return false;
}
void Leave() {
    auto *impl = Peek();
    if (!impl)
        return;
    std::scoped_lock lock(impl->mutex);
    if (impl->active) {
        uint8_t bye[12]{};
        std::memcpy(bye, impl->network_info + 0x20, 6);
        Put32(bye + 8, impl->self);
        impl->LdnUdp(Broadcast, 5, bye, sizeof(bye), true);
    }
    impl->Reset();
}
Snapshot State() {
    auto *impl = Peek();
    Snapshot result;
    if (!impl)
        return result;
    std::scoped_lock lock(impl->mutex);
    result.active = impl->active;
    result.hosting = impl->hosting;
    result.node = impl->node;
    result.address = impl->self;
    result.epoch = impl->epoch;
    result.game = impl->game;
    result.member = impl->own_member;
    result.sent = impl->sent;
    result.received = impl->received;
    for (const auto &peer : impl->peers)
        if (peer.address && peer.admitted)
            result.members++;
    if (impl->joined && (!impl->hosting || !impl->peers[0].admitted))
        result.members++;
    std::memcpy(result.error, impl->error, sizeof(result.error));
    return result;
}
bool GetHostedRoom(HostedRoom *out) {
    if (!out)
        return false;
    *out = {};
    // Keep the instance alive while capturing fields protected by the transport lock.
    std::scoped_lock instance_lock(instance_mutex);
    auto *impl = instance;
    if (!impl)
        return false;
    std::scoped_lock lock(impl->mutex);
    if (!impl->active || !impl->hosting || !impl->published || !impl->joined || impl->collision)
        return false;

    const auto *info = impl->network_info;
    const uint64_t game = uint64_t(Le32(info)) | (uint64_t(Le32(info + 4)) << 32);
    if (!game || game != impl->game || Le32(info + 0x68) != impl->self ||
        !std::any_of(info + 16, info + 32, [](uint8_t byte) { return byte != 0; }) ||
        !std::any_of(info + 0x20, info + 0x26, [](uint8_t byte) { return byte != 0; }))
        return false;

    out->epoch = impl->epoch;
    out->game = game;
    out->node = impl->node;
    out->host_ip = impl->self;
    std::memcpy(out->network_id, info, sizeof(out->network_id));
    std::memcpy(out->bssid, info + 0x20, sizeof(out->bssid));
    if (impl->HostAllows(false)) out->flavors |= 1u << NodeFlavorStandard;
    if (impl->HostAllows(true)) out->flavors |= 1u << NodeFlavorFork;
    return true;
}
void Publish(const void *info, size_t size) {
    auto *impl = Peek();
    if (!impl)
        return;
    std::scoped_lock lock(impl->mutex);
    if (!impl->active || !impl->hosting)
        return;
    if (size == 0) {
        impl->published = false;
        return;
    }
    if (size != InfoBytes || !ValidInfo(static_cast<const uint8_t *>(info)) ||
        Le32(static_cast<const uint8_t *>(info) + 0x68) != impl->self)
        return;
    std::memcpy(impl->network_info, info, size);
    impl->SyncPeers(impl->network_info);
    impl->PinHostFlavor();
    impl->published = true;
    if (impl->HostAllows(true)) {
        uint8_t fork_info[InfoBytes];
        impl->ForkInfo(fork_info);
        impl->LdnUdp(Broadcast, 1, fork_info, size, true);
    } else {
        impl->LdnUdp(Broadcast, 1, info, size, false);
    }
}
bool SendControl(uint8_t destination, uint8_t kind, const void *data, size_t size) {
    auto *impl = Peek();
    if (!impl)
        return false;
    std::scoped_lock lock(impl->mutex);
    if (!impl->active || size > 0x500 || (size && !data))
        return false;
    if (kind == 1 && impl->joined && ValidInfo(impl->network_info)) {
        impl->Queue(0, 2, impl->network_info, InfoBytes);
        return true;
    }
    if (destination >= 8 || !impl->peers[destination].address)
        return false;
    auto &peer = impl->peers[destination];
    if (kind == 3 && !impl->hosting && size == NodeBytes) {
        if (peer.fork) {
            uint8_t body[NodeBytes + 6];
            std::memcpy(body, data, NodeBytes);
            std::memcpy(body + NodeBytes, impl->network_info + 0x20, 6);
            std::memcpy(impl->pending_join, body, sizeof(body));
            impl->join_retry_at = monotonic_ms();
            impl->join_attempts = 1;
            return impl->LdnUdp(Broadcast, 2, body, sizeof(body), true);
        }
        for (auto &flow : impl->flows)
            if (flow.used && flow.ldn && flow.peer == peer.address)
                return impl->WriteLdn(flow, 2, data, size);
    } else if (kind == 4 && impl->hosting && size == InfoBytes) {
        if (peer.fork) {
            uint8_t info[InfoBytes];
            std::memcpy(info, data, size);
            for (unsigned i = 0; i < 8; i++) {
                auto *n = info + 0x68 + 64 * i;
                int member = impl->PeerFor(Le32(n));
                if (n[11] && member >= 0)
                    Put32(n, impl->peers[member].real);
            }
            return impl->LdnUdp(Broadcast, 3, info, size, true);
        }
        for (auto &flow : impl->flows)
            if (flow.used && flow.ldn && flow.peer == peer.address)
                return impl->WriteLdn(flow, 3, data, size);
    } else if (kind == 5) {
        impl->DropPeer(destination);
        return true;
    }
    return false;
}
ssize_t TakeControl(uint8_t *from, uint8_t *kind, void *out, size_t capacity) {
    auto *impl = Peek();
    if (!impl)
        return -1;
    std::scoped_lock lock(impl->mutex);
    if (!impl->control_count)
        return -1;
    auto &entry = impl->controls[impl->control_head];
    if (capacity < entry.size)
        return -1;
    *from = entry.from;
    *kind = entry.kind;
    std::memcpy(out, entry.bytes, entry.size);
    const size_t size = entry.size;
    impl->control_head = (impl->control_head + 1) % impl->controls.size();
    impl->control_count--;
    return size;
}
int MemberOfIp(uint32_t ip) {
    auto *impl = Peek();
    if (!impl)
        return -1;
    std::scoped_lock lock(impl->mutex);
    if (ip == impl->self)
        return impl->own_member;
    return impl->PeerFor(ip);
}
bool SendData(uint32_t destination, uint16_t sport, uint16_t dport, const void *data, size_t size) {
    auto *impl = Peek();
    if (!impl)
        return false;
    std::scoped_lock lock(impl->mutex);
    if (!impl->active || !impl->joined || size > GameBytes)
        return false;
    bool sent = false;
    for (const auto &peer : impl->peers) {
        if (!peer.address || peer.address == impl->self || !peer.admitted ||
            (destination != Broadcast && destination != UINT32_MAX && destination != peer.address))
            continue;
        const bool ok = peer.fork ? impl->ForkData(peer.address, sport, dport, data, size)
                                  : impl->Udp(peer.address, sport, dport, data, size);
        if (!ok)
            return false;
        sent = true;
    }
    if (sent)
        impl->sent++;
    return sent;
}
ssize_t ReceiveData(uint16_t port, void *out, size_t capacity, uint32_t *source, uint16_t *sport, bool peek, uint32_t peer_ip, uint16_t peer_port) {
    auto *impl = Peek();
    if (!impl)
        return -1;
    std::scoped_lock lock(impl->mutex);
    for (size_t i = 0; i < impl->packet_count; i++) {
        const size_t at = (impl->packet_head + i) % impl->packets.size();
        auto &entry = impl->packets[at];
        if (entry.destination_port != port || (peer_ip && (entry.source != peer_ip || entry.source_port != peer_port)))
            continue;
        const size_t size = std::min(capacity, entry.size);
        if (size)
            std::memcpy(out, entry.data, size);
        *source = entry.source;
        *sport = entry.source_port;
        if (!peek) {
            impl->packet_bytes -= entry.size;
            std::free(entry.data);
            for (size_t j = i; j + 1 < impl->packet_count; j++)
                impl->packets[(impl->packet_head + j) % impl->packets.size()] =
                    impl->packets[(impl->packet_head + j + 1) % impl->packets.size()];
            impl->packets[(impl->packet_head + --impl->packet_count) % impl->packets.size()] = {};
        }
        return size;
    }
    errno = EAGAIN;
    return -1;
}
bool HasData(uint16_t port, uint32_t peer_ip, uint16_t peer_port) {
    auto *impl = Peek();
    if (!impl)
        return false;
    std::scoped_lock lock(impl->mutex);
    for (size_t i = 0; i < impl->packet_count; i++) {
        const auto &packet = impl->packets[(impl->packet_head + i) % impl->packets.size()];
        if (packet.destination_port == port && (!peer_ip || (packet.source == peer_ip && packet.source_port == peer_port)))
            return true;
    }
    return false;
}
bool SendStream(uint64_t epoch, const void *body, size_t size) {
    auto *impl = Peek();
    if (!impl)
        return false;
    tcpstream::Frame frame;
    if (!tcpstream::Parse(body, size, &frame))
        return false;
    std::scoped_lock lock(impl->mutex);
    return impl->epoch == epoch && impl->Stream(frame);
}
void SetStreamReply(StreamReply callback) {
    auto *impl = Peek();
    if (!impl)
        return;
    std::scoped_lock lock(impl->mutex);
    impl->stream_reply = callback;
}
void Shutdown() {
    Impl *closing;
    {
        std::scoped_lock lock(instance_mutex);
        closing = instance;
        instance = nullptr;
        terminated = true;
    }
    if (closing)
        closing->~Impl();
}
} // namespace ams::mitm::lanplay
