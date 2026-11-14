/* Existing ldn_mitm framing expressed without socket ownership. GPL-2.0-only. */
#pragma once
#include "../../../common/node_lobby.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ams::mitm::lanplay {
inline uint16_t Le16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t Le32(const uint8_t *p) {
    return Le16(p) | uint32_t(Le16(p + 2)) << 16;
}
inline uint16_t Be16(const uint8_t *p) {
    return uint16_t(p[0]) << 8 | p[1];
}
inline uint32_t Be32(const uint8_t *p) {
    return uint32_t(Be16(p)) << 16 | Be16(p + 2);
}
inline void Put16(uint8_t *p, uint16_t x) {
    p[0] = x;
    p[1] = x >> 8;
}
inline void Put32(uint8_t *p, uint32_t x) {
    Put16(p, x);
    Put16(p + 2, x >> 16);
}
inline void PutBe16(uint8_t *p, uint16_t x) {
    p[0] = x >> 8;
    p[1] = x;
}
inline void PutBe32(uint8_t *p, uint32_t x) {
    PutBe16(p, x >> 16);
    PutBe16(p + 2, x);
}
inline uint32_t Sum(const void *data, size_t length, uint32_t sum = 0) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i + 1 < length; i += 2)
        sum += Be16(bytes + i);
    if (length & 1)
        sum += uint32_t(bytes[length - 1]) << 8;
    return sum;
}
inline uint16_t Fold(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return uint16_t(~sum);
}
inline uint16_t UdpChecksum(uint32_t source, uint32_t destination, const uint8_t *data, size_t bytes) {
    uint8_t pseudo[12]{};
    PutBe32(pseudo, source);
    PutBe32(pseudo + 4, destination);
    pseudo[9] = 17;
    PutBe16(pseudo + 10, bytes);
    return Fold(Sum(data, bytes, Sum(pseudo, sizeof(pseudo))));
}
inline size_t EncodeLdn(uint8_t *out, size_t capacity, uint8_t kind, const void *data, size_t size,
                        uint16_t sequence = 0) {
    if (size > 0x500 || capacity < 12 + size || (size && !data))
        return 0;
    std::memset(out, 0, 12);
    Put32(out, 0x11451400);
    out[4] = kind;
    Put16(out + 6, size);
    Put16(out + 10, sequence);
    if (size)
        std::memcpy(out + 12, data, size);
    return 12 + size;
}
inline bool DecodeLdn(const uint8_t *frame, size_t bytes, uint8_t *kind, uint8_t *out, size_t *size,
                      uint16_t *sequence) {
    if (bytes < 12 || Le32(frame) != 0x11451400 || Le16(frame + 6) != bytes - 12 || frame[5] > 1)
        return false;
    *kind = frame[4];
    *sequence = Le16(frame + 10);
    if (!frame[5]) {
        if (bytes - 12 > *size)
            return false;
        *size = bytes - 12;
        std::memcpy(out, frame + 12, *size);
        return true;
    }
    const size_t expected = Le16(frame + 8);
    if (expected > *size)
        return false;
    size_t consumed = 12, written = 0;
    while (consumed < bytes && written < expected) {
        const uint8_t value = frame[consumed++];
        out[written++] = value;
        if (value == 0) {
            if (consumed == bytes)
                return false;
            const size_t zeroes = frame[consumed++];
            if (zeroes > expected - written)
                return false;
            std::memset(out + written, 0, zeroes);
            written += zeroes;
        }
    }
    *size = written;
    return written == expected && consumed == bytes;
}
inline bool ValidInfo(const uint8_t *info) {
    if (!info || info[0x26] > 32 || !info[0x66] || info[0x66] > 8 || !info[0x67] || info[0x67] > info[0x66] ||
        info[0x73] != 1)
        return false;
    unsigned count = 0;
    for (unsigned i = 0; i < 8; i++) {
        const auto *node = info + 0x68 + 64 * i;
        if (!node[11])
            continue;
        if (node[11] != 1 || node[10] != i || !Le32(node))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (info[0x68 + 64 * j + 11] && Le32(info + 0x68 + 64 * j) == Le32(node))
                return false;
        count++;
    }
    return count == info[0x67];
}
inline bool SameLocator(const NodeRoomLocator &locator, const uint8_t *info, uint32_t host, uint8_t flavor) {
    return locator.host_ip == host && locator.flavor == flavor &&
           !std::memcmp(locator.network_id, info, 32) && !std::memcmp(locator.bssid, info + 0x20, 6);
}
} // namespace ams::mitm::lanplay
