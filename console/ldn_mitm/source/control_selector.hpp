// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace ams::mitm {
    // Evaluated while the queue is locked. The selector reads only its immutable snapshot.
    using ControlSelector = bool (*)(uint8_t from, uint8_t kind, const void *data,
                                     size_t size, const void *context);
}
