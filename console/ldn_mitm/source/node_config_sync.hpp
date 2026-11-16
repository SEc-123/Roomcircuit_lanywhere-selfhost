// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>

namespace ams::mitm::storage {
// Resolve legacy and canonical nodes.ini using durable two-source baselines.
// Conflicts or I/O faults return false without discarding either user's file.
bool ReadNodeConfig(char *out, std::size_t capacity, std::size_t *actual);
}
