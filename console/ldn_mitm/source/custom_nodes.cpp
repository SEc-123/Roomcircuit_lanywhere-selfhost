// SPDX-License-Identifier: GPL-2.0-only
#include "custom_nodes.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <cstring>
#include <strings.h>

namespace ams::mitm::custom_nodes {
namespace {
constexpr size_t MaxConfig = 8192;
uint32_t Read32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Read16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
bool IsZeros(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    return std::all_of(p, p + size, [](uint8_t c) { return c == 0; });
}
bool ValidName(const char *s, size_t capacity) {
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    const auto *end = reinterpret_cast<const unsigned char *>(std::memchr(s, 0, capacity));
    if (!end || end == p) return false;
    for (; p < end;) {
        unsigned c = *p++;
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned n; uint32_t point;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; point = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; point = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; point = c & 0x07; }
        else return false;
        if (static_cast<size_t>(end - p) < n) return false;
        for (unsigned i = 0; i < n; i++) {
            if ((*p & 0xc0) != 0x80) return false;
            point = (point << 6) | (*p++ & 0x3f);
        }
        if ((n == 2 && point < 0x800) || (n == 3 && point < 0x10000) ||
            (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) return false;
    }
    return true;
}
bool NormalizeHost(const char *source, char out[128]) {
    const size_t n = std::strlen(source);
    if (!n || n >= 128) return false;
    size_t end = n;
    if (source[end - 1] == '.') --end;
    if (!end || end > 253) return false;
    size_t label = 0;
    for (size_t i = 0; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (c == '.') {
            if (!label || label > 63 || source[i - 1] == '-') return false;
            out[i] = '.'; label = 0; continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-') || (!label && c == '-')) return false;
        out[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        ++label;
    }
    if (!label || label > 63 || source[end - 1] == '-') return false;
    out[end] = 0;
    return true;
}
bool PaddedString(const char *p, size_t n) {
    const char *end = static_cast<const char *>(std::memchr(p, 0, n));
    return end && IsZeros(end, n - static_cast<size_t>(end - p));
}
bool Add(NodeDynamicCatalog &catalog, const NodeDynamicEntry &entry) {
    if (catalog.count >= sizeof(catalog.nodes) / sizeof(catalog.nodes[0])) return false;
    for (uint32_t i = 0; i < catalog.count; ++i)
        if (catalog.nodes[i].id == entry.id || SameEndpoint(catalog.nodes[i], entry)) return false;
    catalog.nodes[catalog.count++] = entry;
    return true;
}
char *Trim(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    char *end = s + std::strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return s;
}
}

bool PublicIpv4(const uint8_t ip[4]) {
    const unsigned a = ip[0], b = ip[1], c = ip[2];
    return a && a < 224 && a != 127 && a != 10 && !(a == 169 && b == 254) &&
        !(a == 172 && b >= 16 && b <= 31) && !(a == 192 && (b == 168 ||
        (b == 0 && (c == 0 || c == 2)) || (b == 88 && c == 99))) &&
        !(a == 198 && b == 51 && c == 100) && !(a == 203 && b == 0 && c == 113) &&
        !(a == 100 && b >= 64 && b <= 127) && !(a == 198 && (b == 18 || b == 19));
}

bool SameEndpoint(const NodeDynamicEntry &a, const NodeDynamicEntry &b) {
    return a.port == b.port && std::strcmp(a.host, b.host) == 0;
}

bool ParseShared(const void *data, size_t size, const uint8_t legacy_hash[32], NodeDynamicCatalog *out) {
    if (!data || !legacy_hash || !out || size < 40) return false;
    const auto *bytes = static_cast<const uint8_t *>(data);
    const unsigned count = bytes[32];
    const bool with_regions = bytes[33] == NODE_REGION_LABEL_VERSION;
    const size_t row_bytes = sizeof(NodeDynamicEntry) + (with_regions ? NODE_REGION_LABEL_BYTES : 0);
    if (std::memcmp(bytes, legacy_hash, 32) || count > NODE_SHARED_CAPACITY - NODE_DYNAMIC_FIRST ||
        (!with_regions && bytes[33]) || !IsZeros(bytes + 34, 2) || size != 40 + count * row_bytes) return false;
    NodeDynamicCatalog candidate{};
    candidate.version = 1;
    candidate.available = Read32(bytes + 36);
    if (candidate.available & ~((1u << NODE_SHARED_CAPACITY) - 1)) return false;
    uint32_t seen = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto *raw = bytes + 40 + i * row_bytes;
        NodeDynamicEntry node{};
        node.id = Read32(raw); node.protocol = raw[4]; node.region = raw[5];
        node.port = Read16(raw + 8); node.flags = Read16(raw + 10);
        if (node.id < NODE_DYNAMIC_FIRST || node.id >= NODE_SHARED_CAPACITY || node.protocol != 2 ||
            node.region > 5 || node.flags > 1 || !node.port || !IsZeros(raw + 6, 2) ||
            !PaddedString(reinterpret_cast<const char *>(raw + 12), 128) ||
            !PaddedString(reinterpret_cast<const char *>(raw + 140), 48)) return false;
        char host[128]; std::memcpy(host, raw + 12, sizeof(host));
        if (!NormalizeHost(host, node.host)) return false;
        std::memcpy(node.name, raw + 140, sizeof(node.name));
        std::memcpy(node.ipv4, raw + 188, sizeof(node.ipv4));
        const bool available = (candidate.available & (1u << node.id)) != 0;
        const uint8_t *ip = node.ipv4;
        const bool public_ip = PublicIpv4(ip);
        if ((available && !public_ip) || (!available && !IsZeros(ip, 4))) return false;
        if (!ValidName(node.name, sizeof(node.name)) || !Add(candidate, node)) return false;
        if (with_regions) {
            const char *label = reinterpret_cast<const char *>(raw + sizeof(NodeDynamicEntry));
            if (!PaddedString(label, NODE_REGION_LABEL_BYTES) || (*label && !ValidName(label, NODE_REGION_LABEL_BYTES))) return false;
            std::memcpy(candidate.region_labels[node_region_label_index(node.id)], label, NODE_REGION_LABEL_BYTES);
        }
        if (node.flags & 1) seen |= 1u << node.id;
    }
    if (candidate.available & ~seen) return false;
    candidate.enabled = seen;
    *out = candidate;
    return true;
}

bool ParseLocal(const char *text, size_t size, const NodeDynamicCatalog &shared, NodeDynamicCatalog *out) {
    if (!out || (!text && size) || size > MaxConfig || shared.count > NODE_SHARED_CAPACITY - NODE_DYNAMIC_FIRST) return false;
    if (size && std::memchr(text, 0, size)) return false;
    struct Scratch { char input[MaxConfig + 1]; NodeDynamicCatalog result; };
    std::unique_ptr<Scratch, decltype(&std::free)> scratch(static_cast<Scratch *>(std::calloc(1, sizeof(Scratch))), &std::free);
    if (!scratch) return false;
    if (size) std::memcpy(scratch->input, text, size);
    scratch->input[size] = 0;
    scratch->result = shared;
    auto &result = scratch->result;
    NodeDynamicEntry current{};
    char label[NODE_REGION_LABEL_BYTES]{};
    bool section = false, have_name = false, have_host = false, have_port = false, have_region = false;
    unsigned local_count = 0;
    auto finish = [&]() {
        if (!section) return true;
        if (!have_name || !have_host || !have_port || !ValidName(current.name, sizeof(current.name))) return false;
        for (uint32_t i = 0; i < result.count; ++i) {
            if (SameEndpoint(result.nodes[i], current)) {
                // A central node uses its authoritative identity and display name.
                return result.nodes[i].id < NODE_SHARED_CAPACITY;
            }
        }
        if (local_count >= NODE_LOCAL_CAPACITY) return false;
        current.id = NODE_LOCAL_BASE + local_count++;
        current.protocol = 2; current.flags = 1;
        std::memcpy(result.region_labels[node_region_label_index(current.id)], label, sizeof(label));
        return Add(result, current);
    };
    char *line = scratch->input;
    while (line && *line) {
        char *next = std::strchr(line, '\n');
        if (next) *next++ = 0;
        char *item = Trim(line); line = next;
        if (!*item || *item == '#' || *item == ';') continue;
        if (!std::strcmp(item, "[node]")) {
            if (!finish()) return false;
            current = {}; section = true;
            std::memset(label, 0, sizeof(label));
            have_name = have_host = have_port = have_region = false;
            continue;
        }
        if (!section) return false;
        char *equal = std::strchr(item, '=');
        if (!equal) return false;
        *equal++ = 0; char *key = Trim(item); char *value = Trim(equal);
        if (!std::strcmp(key, "name")) {
            if (have_name || std::strlen(value) >= sizeof(current.name)) return false;
            std::strcpy(current.name, value); have_name = true;
        } else if (!std::strcmp(key, "host")) {
            if (have_host || !NormalizeHost(value, current.host)) return false;
            have_host = true;
        } else if (!std::strcmp(key, "port")) {
            if (have_port || !*value) return false;
            unsigned n = 0;
            for (const char *p = value; *p; ++p) {
                if (*p < '0' || *p > '9' || n > 6553) return false;
                n = n * 10 + static_cast<unsigned>(*p - '0');
            }
            if (!n || n > 65535) return false;
            current.port = static_cast<uint16_t>(n); have_port = true;
        } else if (!std::strcmp(key, "region")) {
            if (have_region) return false;
            static const char *const regions[] = {"unknown", "asia", "europe", "americas", "oceania", "africa"};
            const auto it = std::find_if(std::begin(regions), std::end(regions), [&](const char *s) { return !strcasecmp(s, value); });
            if (it != std::end(regions)) current.region = static_cast<uint8_t>(it - std::begin(regions));
            else if (!strcasecmp(value, "China") || !strcasecmp(value, "CN") ||
                     !std::strcmp(value, "中国") || !std::strcmp(value, "中國")) {
                current.region = 1;
                std::strcpy(label, "China");
            } else if (*value) {
                if (!ValidName(value, std::min(std::strlen(value) + 1, sizeof(label)))) return false;
                std::strcpy(label, value);
            }
            have_region = true;
        } else return false;
    }
    if (!finish()) return false;
    *out = result;
    return true;
}
}
