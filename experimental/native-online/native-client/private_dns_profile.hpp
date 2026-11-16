#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace roomcircuit::private_dns {

    using Profile = std::uint16_t;
    constexpr std::string_view Header = "RoomCircuitNativeDNS1 49.233.147.207\n";
    constexpr std::uint32_t Endpoint = 49u | (233u << 8) | (147u << 16) | (207u << 24);
    constexpr std::size_t MaxBytes = 1024;
    constexpr std::array<std::string_view, 11> Hosts = {
        "accounts.nintendo.com",
        "api.accounts.nintendo.com",
        "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com",
        "m-lp1.baas.nintendo.com",
        "capi.lp1.op2.nintendo.net",
        "dauth-lp1.ndas.srv.nintendo.net",
        "dcert-lp1.ndas.srv.nintendo.net",
        "aauth-lp1.ndas.srv.nintendo.net",
        "aauth.hac.lp1.ndas.srv.nintendo.net",
        "g2c3c9a00-lp1.s.n.srv.nintendo.net",
        "g28abaa00-lp1.s.n.srv.nintendo.net",
    };
    static_assert(Hosts.size() <= 8 * sizeof(Profile));

    constexpr char AsciiLower(char c) {
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }

    constexpr Profile HostBit(std::string_view name, bool query) {
        if (query && name.ends_with('.')) {
            name.remove_suffix(1);
        }
        for (std::size_t index = 0; index < Hosts.size(); ++index) {
            const auto candidate = Hosts[index];
            if (name.size() != candidate.size()) {
                continue;
            }
            bool equal = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if ((query ? AsciiLower(name[i]) : name[i]) != candidate[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                return static_cast<Profile>(1u << index);
            }
        }
        return 0;
    }

    constexpr Profile Parse(std::string_view file) {
        if (file.size() > MaxBytes || !file.starts_with(Header)) {
            return 0;
        }
        file.remove_prefix(Header.size());
        Profile result = 0;
        while (!file.empty()) {
            const auto end = file.find('\n');
            if (end == std::string_view::npos) {
                return 0;
            }
            const auto bit = HostBit(file.substr(0, end), false);
            if (bit == 0 || (result & bit) != 0) {
                return 0;
            }
            result |= bit;
            file.remove_prefix(end + 1);
        }
        return result;
    }

    constexpr bool Matches(Profile active, const char *name, std::size_t size) {
        if (active == 0 || name == nullptr) {
            return false;
        }
        const std::string_view buffer(name, size < 254 ? size : 254);
        const auto end = buffer.find('\0');
        return end != std::string_view::npos && (active & HostBit(buffer.substr(0, end), true)) != 0;
    }

    constexpr bool ParseService(std::string_view service, std::uint16_t &port) {
        if (service.empty()) {
            return false;
        }
        if (service == "https") {
            port = 443;
            return true;
        }
        if (service.size() > 5) {
            return false;
        }
        std::uint32_t number = 0;
        for (const auto c : service) {
            if (c < '0' || c > '9') {
                return false;
            }
            number = 10 * number + static_cast<std::uint32_t>(c - '0');
        }
        if (number > 65535) {
            return false;
        }
        port = static_cast<std::uint16_t>(number);
        return true;
    }

    struct AddrInfoHint {
        std::uint32_t flags;
        std::uint32_t family;
        std::uint32_t socket_type;
        std::uint32_t protocol;
    };

    constexpr std::uint32_t ReadBigEndianWord(std::string_view bytes, std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
        }
        return value;
    }

    constexpr bool ParseAddrInfoHint(std::string_view bytes, AddrInfoHint &hint) {
        constexpr std::uint32_t AddrInfoMagic = 0xBEEFCAFE;
        // A resolver hint has no address, canonical name or linked node.
        constexpr std::size_t BareHintSize = 6 * sizeof(std::uint32_t) + 2 * sizeof(std::uint32_t) + 1;
        if (bytes.size() != BareHintSize || ReadBigEndianWord(bytes, 0) != AddrInfoMagic) {
            return false;
        }
        for (auto value : bytes.substr(5 * sizeof(std::uint32_t))) {
            if (value != '\0') {
                return false;
            }
        }
        hint = {ReadBigEndianWord(bytes, 4), ReadBigEndianWord(bytes, 8),
                ReadBigEndianWord(bytes, 12), ReadBigEndianWord(bytes, 16)};
        return true;
    }

}
