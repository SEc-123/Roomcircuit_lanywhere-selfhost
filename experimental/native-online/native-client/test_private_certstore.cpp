// SPDX-License-Identifier: GPL-2.0-only
#include "private_certstore_format.hpp"

#include <cassert>
#include <cstdlib>
#include <memory>
#include <vector>

extern "C" {
#include "../native-validation/trust-store/store.h"
}

using ams::mitm::fs::private_certstore::MatchDigest;
using ams::mitm::fs::private_certstore::ValidateImage;

static void store32(std::uint8_t *out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

int main() {
    constexpr unsigned count = 63;
    constexpr unsigned tableSize = count * 16;
    std::vector<std::uint8_t> bdf(8 + tableSize + count);
    store32(bdf.data(), 0x546c7373);
    store32(bdf.data() + 4, count);
    for (unsigned i = 0; i < count; ++i) {
        auto *entry = bdf.data() + 8 + i * 16;
        store32(entry, i + 1);
        store32(entry + 8, 1);
        store32(entry + 12, tableSize + i);
    }

    std::uint8_t *raw = nullptr;
    std::size_t size = 0;
    assert(rc_store_romfs(bdf.data(), bdf.size(), &raw, &size) == 0);
    std::unique_ptr<std::uint8_t, decltype(&std::free)> image(raw, &std::free);
    assert(ValidateImage(image.get(), size));
    assert(!ValidateImage(nullptr, size));
    assert(!ValidateImage(image.get(), size - 1));
    assert(!ValidateImage(image.get(), ams::mitm::fs::private_certstore::MaxImageSize + 1));

    for (const auto offset : {32, 92, 160, 192, 196, 208, 212}) {
        image.get()[offset] ^= 0xff;
        assert(!ValidateImage(image.get(), size));
        image.get()[offset] ^= 0xff;
        assert(ValidateImage(image.get(), size));
    }

    std::uint8_t digest[32] = {};
    char sidecar[65];
    std::memset(sidecar, '0', sizeof(sidecar));
    sidecar[64] = '\n';
    assert(MatchDigest(sidecar, digest));
    sidecar[0] = 'A';
    assert(!MatchDigest(sidecar, digest));
    sidecar[0] = '0';
    sidecar[64] = ' ';
    assert(!MatchDigest(sidecar, digest));
}
