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
    assert(argc == 3);
    const bool standard_first = std::strcmp(argv[1], "standard") == 0;
    lanplay::Endpoint endpoint{htonl(INADDR_LOOPBACK), htons(std::atoi(argv[2]))};
    assert(lanplay::Configure(2, endpoint) && lanplay::Host(Game));
    auto info = Network(lanplay::State().address);
    lanplay::Publish(info.data(), info.size());
    std::printf("READY %u\n", lanplay::State().address);
    std::fflush(stdout);
    for (unsigned round = 0; round < 2; round++) {
        unsigned accepted = 0;
        const auto begin = std::chrono::steady_clock::now();
        const auto duration = std::chrono::seconds(round ? 4 : standard_first ? 23 : 4);
        const auto original = lanplay::State();
        while (std::chrono::steady_clock::now() - begin < duration) {
            uint8_t from, kind, body[0x500];
            ssize_t bytes;
            while ((bytes = lanplay::TakeControl(&from, &kind, body, sizeof(body))) >= 0) {
                if (kind != 3 || bytes != 64 || lanplay::MemberOfIp(lanplay::Le32(body)) != from)
                    continue;
                assert(++accepted == 1);
                std::memcpy(info.data() + 0xa8, body, 64);
                info[0xb2] = 1;
                info[0x67] = 2;
                lanplay::Publish(info.data(), info.size());
                assert(lanplay::SendControl(from, 4, info.data(), info.size()));
                std::printf("ACCEPT %u\n", round);
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto state = lanplay::State();
        assert(accepted == 1 && state.active && state.members == 2 && state.epoch == original.epoch);
        if (!round) {
            std::puts("LOCKED");
            lanplay::Leave();
            assert(lanplay::Configure(2, endpoint) && lanplay::Host(Game));
            info = Network(lanplay::State().address);
            info[0x10] = 4;
            lanplay::Publish(info.data(), info.size());
            std::printf("RESET %u\n", lanplay::State().address);
            std::fflush(stdout);
        }
    }
    lanplay::Shutdown();
    std::puts("PASS incompatible visitor rejected; room rebuild resets protocol lock");
}
