// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../../../common/node_lobby.h"
#include <cstddef>
#include <cstdint>

namespace ams::mitm::custom_nodes {

// Nodes from one snapshot are accepted atomically. Returns false without
// modifying output if any record is malformed or conflicts with another.
bool ParseShared(const void *data, size_t size, const uint8_t legacy_hash[32], NodeDynamicCatalog *out);
bool ParseLocal(const char *text, size_t size, const NodeDynamicCatalog &shared, NodeDynamicCatalog *out);
bool PublicIpv4(const uint8_t address[4]);
bool SameEndpoint(const NodeDynamicEntry &a, const NodeDynamicEntry &b);

}
