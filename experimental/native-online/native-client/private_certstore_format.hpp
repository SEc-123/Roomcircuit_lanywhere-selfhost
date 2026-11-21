// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ams::mitm::fs::private_certstore {

inline constexpr std::size_t MaxImageSize = 256 * 1024;
inline constexpr std::size_t SidecarSize = 65;
inline constexpr std::size_t DataOffset = 192;
inline constexpr char ImagePath[] = "roomcircuit-native/CertStore/romfs.bin";
inline constexpr char DigestPath[] = "roomcircuit-native/CertStore/romfs.sha256";

template <typename UInt>
UInt ReadLittleEndian(const std::uint8_t *data) {
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value |= static_cast<UInt>(data[index]) << (8 * index);
    }
    return value;
}

template <typename UInt, std::size_t Count>
bool MatchFields(const std::uint8_t *data, const UInt (&expected)[Count]) {
    for (std::size_t index = 0; index < Count; ++index) {
        if (ReadLittleEndian<UInt>(data + index * sizeof(UInt)) != expected[index]) {
            return false;
        }
    }
    return true;
}

inline bool MatchDigest(const char (&sidecar)[SidecarSize], const std::uint8_t (&digest)[32]) {
    constexpr char hex[] = "0123456789abcdef";
    if (sidecar[SidecarSize - 1] != '\n') {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(digest); ++index) {
        if (sidecar[2 * index] != hex[digest[index] >> 4] || sidecar[2 * index + 1] != hex[digest[index] & 15]) {
            return false;
        }
    }
    return true;
}

inline bool ValidateImage(const std::uint8_t *data, std::size_t size) {
    constexpr std::uint32_t empty = UINT32_MAX;
    constexpr std::uint64_t layout[] = {80, 80, 12, 92, 24, 116, 12, 128, 52, DataOffset};
    constexpr std::uint32_t dirHash[] = {0, empty, empty};
    constexpr std::uint32_t dirFields[] = {0, empty, empty, 0, empty, 0};
    constexpr std::uint32_t fileHash[] = {empty, empty, 0};
    constexpr std::uint32_t fileFields[] = {0, empty};
    constexpr std::uint32_t fileEnd[] = {empty, 20};
    constexpr char name[] = "ssl_TrustedCerts.bdf";
    constexpr std::size_t entryCount = 63;
    constexpr std::size_t bdfHeaderSize = 8 + entryCount * 16;

    if (data == nullptr || size < DataOffset + bdfHeaderSize || size > MaxImageSize) {
        return false;
    }
    const std::uint64_t fileRange[] = {0, size - DataOffset};
    if (!MatchFields(data, layout) || !MatchFields(data + 80, dirHash) ||
        !MatchFields(data + 92, dirFields) || !MatchFields(data + 116, fileHash) ||
        !MatchFields(data + 128, fileFields) || !MatchFields(data + 136, fileRange) ||
        !MatchFields(data + 152, fileEnd) || std::memcmp(data + 160, name, sizeof(name) - 1) != 0) {
        return false;
    }

    const auto *bdf = data + DataOffset;
    const std::size_t payloadSize = size - DataOffset - 8;
    if (ReadLittleEndian<std::uint32_t>(bdf) != 0x546c7373 ||
        ReadLittleEndian<std::uint32_t>(bdf + 4) != entryCount) {
        return false;
    }

    for (std::size_t index = 0; index < entryCount; ++index) {
        const auto *entry = bdf + 8 + index * 16;
        const auto length = ReadLittleEndian<std::uint32_t>(entry + 8);
        const auto offset = ReadLittleEndian<std::uint32_t>(entry + 12);
        if (length == 0 || offset < entryCount * 16 || offset > payloadSize || length > payloadSize - offset) {
            return false;
        }
    }
    return true;
}

}
