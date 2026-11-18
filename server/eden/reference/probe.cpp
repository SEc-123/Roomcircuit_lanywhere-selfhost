// SPDX-License-Identifier: GPL-2.0-or-later
// Drives the actual upstream RoomMember API. Game/HLE calls are supplied here.
#include "network/room_member.h"
#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

using namespace Network;
static_assert(static_cast<u8>(Protocol::UDP) == REFERENCE_UDP);
using namespace std::chrono_literals;

static void require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "Eden reference: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    require(argc == 5 && enet_initialize() == 0, "expected port, outbound and inbound files");
    const bool hosting = std::string(argv[4]) == "host";
    std::ifstream input(argv[2], std::ios::binary);
    std::vector<u8> outbound{std::istreambuf_iterator<char>(input), {}};
    require(!outbound.empty(), "missing compressed UDP payload");
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<LDNPacket> control;
    std::vector<ProxyPacket> game;
    RoomMember member;
    member.BindOnLdnPacketReceived([&](const LDNPacket& packet) {
        std::lock_guard guard(mutex);
        control.push_back(packet);
        changed.notify_one();
    });
    member.BindOnProxyPacketReceived([&](const ProxyPacket& packet) {
        std::lock_guard guard(mutex);
        game.push_back(packet);
        changed.notify_one();
    });
    member.Join(hosting ? "ReferenceHost" : "ReferenceGuest", "127.0.0.1", static_cast<u16>(std::stoi(argv[1])));
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (member.GetState() != RoomMember::State::Joined && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(10ms);
    require(member.GetState() == RoomMember::State::Joined, "room handshake");
    require(member.GetRoomInformation().name == "RoomCircuit" && member.GetMemberInformation().size() == 1,
            "upstream room information parsing");
    const auto ip = member.GetFakeIpAddress();
    require(ip[0] == 10 && ip[1] == 13, "native game address");
    member.SendGameInfo({"reference game", 0x0100000000abc000, "1"});
    auto receive = [&](LDNPacketType type) {
        std::unique_lock lock(mutex);
        auto find = [&]() { return std::find_if(control.begin(), control.end(),
            [&](const auto& p) { return p.type == type; }); };
        require(changed.wait_for(lock, 5s, [&]() { return find() != control.end(); }), "LDN response");
        auto iter = find();
        auto result = std::move(*iter);
        control.erase(iter);
        return result;
    };
    std::vector<u8> node(0x40);
    std::reverse_copy(ip.begin(), ip.end(), node.begin());
    std::copy(ip.begin(), ip.end(), node.begin() + 6);
    node[4] = 2;
    node[11] = 1;
    node[46] = 42;
    IPv4Address remote;
    if (hosting) {
        const auto poll = receive(LDNPacketType::Scan);
        std::vector<u8> info(0x480);
        const std::array<u8, 8> game_id{0x00, 0xc0, 0xab, 0x00, 0x00, 0x00, 0x00, 0x01};
        std::copy(game_id.begin(), game_id.end(), info.begin());
        info[0x10] = 77;
        std::copy(node.begin() + 4, node.begin() + 10, info.begin() + 0x20);
        info[0x66] = 8;
        info[0x67] = 1;
        std::copy(node.begin(), node.end(), info.begin() + 0x68);
        member.SendLdnPacket({LDNPacketType::ScanResp, ip, poll.local_ip, false, info});
        const auto joined = receive(LDNPacketType::Connect);
        require(joined.data.size() == 0x40 && joined.remote_ip == ip, "Switch join");
        remote = joined.local_ip;
        info[0x67] = 2;
        std::copy(joined.data.begin(), joined.data.end(), info.begin() + 0xa8);
        info[0xa8 + 10] = 1;
        member.SendLdnPacket({LDNPacketType::SyncNetwork, ip, remote, false, info});
    } else {
        member.SendLdnPacket({LDNPacketType::Scan, ip, {}, true, {}});
        const auto found = receive(LDNPacketType::ScanResp);
        require(found.data.size() == 0x480 && found.remote_ip == ip, "Switch host discovery");
        remote = found.local_ip;
        member.SendLdnPacket({LDNPacketType::Connect, ip, remote, false, node});
        const auto sync = receive(LDNPacketType::SyncNetwork);
        require(sync.local_ip == remote && sync.data.size() == 0x480 && sync.data[0x67] == 2, "Switch admission");
    }
    // Verify that the unsupported TCP enum on this profile is never delivered
    // as a Switch game datagram. ENet sends both frames in order.
    member.SendProxyPacket({{Domain::INET, ip, 3455}, {Domain::INET, remote, 7788}, Protocol::TCP, false, outbound});
    member.SendProxyPacket({{Domain::INET, ip, 3456}, {Domain::INET, remote, 7788}, Protocol::UDP, false, outbound});
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 5s, [&]() { return !game.empty(); }), "native reply");
        const auto& packet = game.front();
        require(packet.local_endpoint.ip == remote && packet.remote_endpoint.ip == ip &&
            packet.local_endpoint.portno == 3344 && packet.remote_endpoint.portno == 7788 &&
            packet.protocol == Protocol::UDP && !packet.broadcast, "native reply fields");
        std::ofstream output(argv[3], std::ios::binary);
        output.write(reinterpret_cast<const char*>(packet.data.data()), packet.data.size());
        require(output.good(), "output file");
    }
    if (hosting)
        member.SendLdnPacket({LDNPacketType::DestroyNetwork, ip, remote, false, {}});
    else
        member.SendLdnPacket({LDNPacketType::Disconnect, ip, remote, false, node});
    member.Leave();
    std::printf("Upstream RoomMember %s: parsed room and LDN frames, exchanged UDP proxy frames and left\n", argv[4]);
}
