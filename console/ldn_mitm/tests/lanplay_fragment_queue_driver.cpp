#include "../source/lanplay_direct.hpp"
#include "../source/lanplay_codec.hpp"
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace ams::mitm;
static constexpr uint64_t Game = 0x0100123456789000ULL;
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

int main(int argc, char **argv) {
    assert(argc == 2);
    lanplay::Endpoint endpoint{htonl(INADDR_LOOPBACK), htons(std::atoi(argv[1]))};
    assert(lanplay::Configure(2, endpoint) && lanplay::Host(Game));
    auto info = Network(lanplay::State().address);
    lanplay::Publish(info.data(), info.size());
    std::printf("READY %u\n", lanplay::State().address); std::fflush(stdout);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool echoed = false;
    while (std::chrono::steady_clock::now() < end && !echoed) {
        uint8_t from, kind, body[0x500]; ssize_t bytes;
        while ((bytes = lanplay::TakeControl(&from, &kind, body, sizeof(body))) >= 0) {
            if (kind != 3 || bytes != 64) continue;
            std::memcpy(info.data() + 0xa8, body, 64); info[0xb2] = 1; info[0x67] = 2;
            lanplay::Publish(info.data(), info.size());
            assert(lanplay::SendControl(from, 4, info.data(), info.size()));
        }
        std::array<uint8_t, 65507> packet{}; uint32_t source; uint16_t port;
        bytes = lanplay::ReceiveData(45602, packet.data(), packet.size(), &source, &port, false);
        if (bytes >= 0) {
            assert(bytes == 65507);
            assert(lanplay::SendData(source, 45602, port, packet.data(), bytes));
            echoed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(echoed);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    lanplay::Shutdown();
}
