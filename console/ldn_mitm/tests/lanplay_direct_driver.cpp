#include "../source/lanplay_codec.hpp"
#include "../source/lanplay_direct.hpp"
#include "../source/tcp_stream_protocol.hpp"
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>
using namespace ams::mitm;
static uint64_t Game = 0x0100123456789000ULL;
static std::atomic<unsigned> stream_open{0}, stream_bytes{0};
static std::deque<std::vector<uint8_t>> echo_pending;
static uint64_t echo_sent = 0, echo_ack = 0;
static tcpstream::Frame echo_frame;
static void FlushEcho(uint64_t epoch) {
    while (!echo_pending.empty() && echo_pending.front().size() <= 8192 - (echo_sent - echo_ack)) {
        auto &payload = echo_pending.front();
        echo_frame.op = tcpstream::Op::Data;
        echo_frame.owner = 1;
        echo_frame.seq = echo_sent;
        echo_frame.source_port = echo_frame.destination_port = echo_frame.aux = 0;
        echo_frame.data = payload.data();
        echo_frame.size = payload.size();
        uint8_t bytes[1100];
        const size_t n = tcpstream::Encode(bytes, sizeof(bytes), echo_frame);
        assert(n && lanplay::SendStream(epoch, bytes, n));
        echo_sent += payload.size();
        echo_pending.pop_front();
    }
}
static std::atomic<bool> own_open{false};
static std::atomic<unsigned> own_reply{0};
static void Stream(uint64_t epoch, const void *data, size_t size) {
    tcpstream::Frame frame{};
    assert(tcpstream::Parse(data, size, &frame));
    uint8_t buffer[1100];
    tcpstream::Frame reply = frame;
    reply.owner = frame.owner ? 0 : 1;
    reply.data = nullptr;
    reply.size = 0;
    reply.source_port = reply.destination_port = 0;
    reply.seq = 0;
    if (frame.op == tcpstream::Op::Open) {
        reply.op = tcpstream::Op::OpenOk;
        reply.aux = 8192;
        stream_open++;
    } else if (frame.op == tcpstream::Op::OpenOk || frame.op == tcpstream::Op::LegacyReady) {
        stream_open++;
        if (frame.id == 99)
            own_open = true;
        return;
    } else if (frame.op == tcpstream::Op::Data) {
        stream_bytes += frame.size;
        if (frame.owner == 1)
            own_reply += frame.size;
        if (frame.owner == 0) {
            echo_frame = frame;
            echo_pending.emplace_back(frame.data, frame.data + frame.size);
            FlushEcho(epoch);
        }
        reply.op = tcpstream::Op::Window;
        reply.seq = frame.seq + frame.size;
        reply.data = nullptr;
        reply.size = 0;
        reply.aux = 8192;
    } else if (frame.op == tcpstream::Op::Window) {
        if (frame.owner == 0) {
            echo_ack = frame.seq;
            FlushEcho(epoch);
        }
        return;
    } else if (frame.op == tcpstream::Op::Fin) {
        reply.op = tcpstream::Op::Fin;
        reply.seq = frame.seq;
        reply.aux = 0;
    } else
        return;
    const size_t n = tcpstream::Encode(buffer, sizeof(buffer), reply);
    assert(n && lanplay::SendStream(epoch, buffer, n));
}
static std::array<uint8_t, 0x480> Network(uint32_t address) {
    std::array<uint8_t, 0x480> result{};
    for (unsigned i = 0; i < 8; i++)
        result[i] = Game >> (8 * i);
    result[0x10] = 3;
    result[0x1f] = 9;
    result[0x20] = 2;
    lanplay::Put32(result.data() + 0x22, address);
    result[0x66] = 8;
    result[0x67] = 1;
    lanplay::Put32(result.data() + 0x68, address);
    result[0x6c] = 2;
    lanplay::Put32(result.data() + 0x6e, address);
    result[0x73] = 1;
    std::memcpy(result.data() + 0x74, "Native host", 12);
    lanplay::Put16(result.data() + 0x96, 1);
    return result;
}
struct Found {
    char code[8]{};
    std::array<uint8_t, 0x480> info{};
};
static void FoundGroup(const char *code, const void *raw, void *arg) {
    const auto *info = static_cast<const uint8_t *>(raw);
    uint64_t id = 0;
    for (unsigned i = 0; i < 8; i++)
        id |= uint64_t(info[i]) << (i * 8);
    if (id != Game)
        return;
    auto *found = static_cast<Found *>(arg);
    std::snprintf(found->code, 8, "%s", code);
    std::memcpy(found->info.data(), info, found->info.size());
}
int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    if (const char *game = std::getenv("LANPLAY_TEST_GAME")) {
        Game = std::strtoull(game, nullptr, 0);
        assert(Game);
    }
    const bool hosting = std::strstr(argv[1], "host") != nullptr;
    const unsigned port = std::atoi(argv[2]);
    lanplay::Endpoint endpoint{htonl(INADDR_LOOPBACK), htons(port)};
    if (argc == 4)
        assert(inet_pton(AF_INET, argv[3], &endpoint.ipv4) == 1);
    const bool probe_reachable = !std::strcmp(argv[1], "probe");
    if (probe_reachable || !std::strcmp(argv[1], "probe-unreachable")) {
        const auto start = std::chrono::steady_clock::now();
        const bool reachable = lanplay::Probe(endpoint);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        assert(reachable == probe_reachable);
        assert(elapsed < std::chrono::milliseconds(1800));
        assert(reachable || elapsed >= std::chrono::milliseconds(1450));
        return 0;
    }
    assert(lanplay::Configure(2, endpoint));
    if (!std::strcmp(argv[1], "hostidentity")) {
        lanplay::HostedRoom snapshot;
        assert(!lanplay::GetHostedRoom(&snapshot));
        assert(lanplay::Host(Game));
        assert(!lanplay::GetHostedRoom(&snapshot));
        const auto state = lanplay::State();
        auto info = Network(state.address);
        lanplay::Publish(info.data(), info.size());
        assert(lanplay::GetHostedRoom(&snapshot));
        assert(snapshot.epoch == state.epoch && snapshot.node == state.node &&
               snapshot.game == Game && snapshot.host_ip == state.address);
        assert(!std::memcmp(snapshot.network_id, info.data(), sizeof(snapshot.network_id)));
        assert(!std::memcmp(snapshot.bssid, info.data() + 0x20, sizeof(snapshot.bssid)));
        assert(snapshot.flavors == ((1u << NodeFlavorStandard) | (1u << NodeFlavorFork)));
        lanplay::Publish(nullptr, 0);
        assert(!lanplay::GetHostedRoom(&snapshot) && !snapshot.epoch);
        lanplay::Leave();
        assert(lanplay::Host(Game));
        info[0x10]++;
        lanplay::Publish(info.data(), info.size());
        assert(lanplay::GetHostedRoom(&snapshot) && snapshot.epoch != state.epoch);
        assert(!std::memcmp(snapshot.network_id, info.data(), sizeof(snapshot.network_id)));
        lanplay::Leave();
        assert(!lanplay::GetHostedRoom(&snapshot) && !snapshot.epoch);
        lanplay::Shutdown();
        assert(!lanplay::GetHostedRoom(&snapshot));
        std::puts("PASS host identity follows publication and exact transport session");
        return 0;
    }
    if (!std::strncmp(argv[1], "scan", 4)) {
        const auto target = Network(0x0a0d7733);
        NodeRoomLocator locator{};
        std::memcpy(locator.network_id, target.data(), 32);
        std::memcpy(locator.bssid, target.data() + 0x20, 6);
        locator.flavor = NodeFlavorStandard;
        locator.host_ip = 0x0a0d7733;
        std::memcpy(locator.code, "TARGET", 7);
        uint8_t filter[96]{};
        std::memcpy(filter, target.data(), 8);
        lanplay::Put32(filter + 92, 1);
        Found found;
        const bool exact = !std::strcmp(argv[1], "scanexact");
        const int count =
            lanplay::Scan(filter, sizeof(filter), exact ? &locator : nullptr, FoundGroup, &found);
        assert(count == 1 && found.code[0]);
        assert(std::memcmp(found.info.data(), target.data(), 32) == 0);
        if (exact)
            assert(std::strcmp(found.code, "TARGET") == 0);
        std::puts("PASS scan filters before capacity and exact locator preserved");
        lanplay::Shutdown();
        return 0;
    }
    if (!std::strcmp(argv[1], "collision") || !std::strcmp(argv[1], "rebind")) {
        assert(lanplay::Host(Game));
        auto info = Network(lanplay::State().address);
        lanplay::Publish(info.data(), info.size());
        const auto original = lanplay::State();
        std::printf("READY %u\n", original.address);
        std::fflush(stdout);
        if (!std::strcmp(argv[1], "collision")) {
            for (unsigned i = 0; i < 200 && lanplay::State().active; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            assert(!lanplay::State().active);
            assert(std::strstr(lanplay::State().error, "address conflict"));
            lanplay::Leave();
            assert(lanplay::Configure(2, endpoint) && lanplay::Host(Game));
            assert(lanplay::State().address != original.address);
            std::puts("PASS collision ends old session and renews address before reopen");
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(24));
            const auto current = lanplay::State();
            assert(current.active && current.epoch == original.epoch && current.address == original.address &&
                   current.node == original.node);
            std::puts("PASS UDP socket rebind preserves node address and epoch");
        }
        lanplay::Shutdown();
        return 0;
    }
    lanplay::SetStreamReply(Stream);
    std::array<uint8_t, 0x480> info{};
    if (hosting) {
        assert(lanplay::Host(Game));
        info = Network(lanplay::State().address);
        lanplay::Publish(info.data(), info.size());
    } else {
        Found found;
        uint8_t filter[96]{};
        for (unsigned i = 0; i < 8; i++) filter[i] = Game >> (8 * i);
        lanplay::Put32(filter + 92, 1);
        for (unsigned i = 0; i < 10 && !found.code[0]; i++)
            assert(lanplay::Scan(filter, sizeof(filter), nullptr, FoundGroup, &found) >= 0);
        assert(found.code[0] && lanplay::Join(found.code, Game));
        info = found.info;
        uint8_t node[64]{};
        lanplay::Put32(node, lanplay::State().address);
        node[4] = 2;
        node[11] = 1;
        std::memcpy(node + 12, "Native guest", 13);
        lanplay::Put16(node + 46, 1);
        assert(lanplay::SendControl(0, 3, node, sizeof(node)));
    }
    std::printf("READY %u\n", lanplay::State().address);
    std::fflush(stdout);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::getenv("LANPLAY_TEST_ENDPOINT") ? 25 : 12);
    unsigned controls = 0, udp = 0;
    bool sent_open = false, sent_payload = false;
    while (std::chrono::steady_clock::now() < until) {
        uint8_t from, kind, data[0x500];
        ssize_t bytes;
        while ((bytes = lanplay::TakeControl(&from, &kind, data, sizeof(data))) >= 0) {
            if (hosting && kind == 3 && bytes == 64) {
                const unsigned slot = info[0x67]++;
                assert(slot < 8);
                std::memcpy(info.data() + 0x68 + slot * 64, data, 64);
                info[0x68 + slot * 64 + 10] = slot;
                lanplay::Publish(info.data(), info.size());
                lanplay::HostedRoom hosted;
                assert(lanplay::GetHostedRoom(&hosted));
                assert(hosted.flavors == (1u << (!std::strcmp(argv[1], "forkhost") ?
                                                NodeFlavorFork : NodeFlavorStandard)));
                assert(!std::memcmp(hosted.network_id, info.data(), sizeof(hosted.network_id)));
                assert(lanplay::SendControl(from, 4, info.data(), info.size()));
                controls++;
            } else if (!hosting && kind == 4 && bytes == 0x480) {
                std::memcpy(info.data(), data, bytes);
                controls++;
            }
        }
        std::array<uint8_t, 65507> packet{};
        uint32_t source;
        uint16_t source_port;
        while ((bytes = lanplay::ReceiveData(45602, packet.data(), packet.size(), &source, &source_port,
                                             false)) >= 0) {
            assert(lanplay::SendData(source, 45602, source_port, packet.data(), bytes));
            udp++;
        }
        if (controls && !sent_open) {
            const auto state = lanplay::State();
            tcpstream::Frame frame{};
            frame.peer = lanplay::Le32(info.data() + (hosting ? 0xa8 : 0x68));
            frame.id = 99;
            frame.nonce = state.epoch;
            frame.op = tcpstream::Op::Open;
            frame.source_port = 50123;
            frame.destination_port = 6501;
            frame.aux = 8192;
            frame.data = info.data();
            frame.size = 32;
            uint8_t wire[128];
            const size_t n = tcpstream::Encode(wire, sizeof(wire), frame);
            assert(n && lanplay::SendStream(state.epoch, wire, n));
            sent_open = true;
            if (!hosting) {
                // The accepted game stream confirms the host's SYNC has been applied.
                std::puts("ADMITTED");
                std::fflush(stdout);
            }
        }
        if (own_open && !sent_payload) {
            const auto state = lanplay::State();
            tcpstream::Frame frame{};
            frame.peer = lanplay::Le32(info.data() + (hosting ? 0xa8 : 0x68));
            frame.id = 99;
            frame.nonce = state.epoch;
            frame.op = tcpstream::Op::Data;
            static const uint8_t payload[] = "native-to-reference-game-TCP";
            frame.data = payload;
            frame.size = sizeof(payload);
            uint8_t wire[128];
            const size_t n = tcpstream::Encode(wire, sizeof(wire), frame);
            assert(n && lanplay::SendStream(state.epoch, wire, n));
            sent_payload = true;
        }
        if (controls && udp >= 3 && own_reply >= 28 && stream_bytes >= 16384) {
            std::printf("PASS control=%u UDP=%u\n", controls, udp);
            std::fflush(stdout);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!(controls && udp >= 3 && own_reply >= 28 && stream_bytes >= 16384))
        std::fprintf(stderr, "TIMEOUT controls=%u UDP=%u own_reply=%u stream_bytes=%u\n", controls, udp, own_reply.load(), stream_bytes.load());
    assert(controls && udp >= 3 && own_reply >= 28 && stream_bytes >= 16384);
    // Allow the adapter's bounded fragmented-datagram queue to drain before this test host closes.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    lanplay::Leave();
    lanplay::Shutdown();
}
