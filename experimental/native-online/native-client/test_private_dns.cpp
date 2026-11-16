#include "private_dns_profile.hpp"

#include <cassert>
#include <string>

using namespace roomcircuit::private_dns;

int main() {
    std::string complete(Header);
    for (auto host : Hosts) {
        complete.append(host).push_back('\n');
    }
    const auto all = Parse(complete);
    assert(all != 0);
    for (auto host : Hosts) {
        const std::string name(host);
        assert(Matches(all, name.c_str(), name.size() + 1));
        assert(!Matches(all, name.c_str(), name.size()));
    }
    const auto one = Parse(std::string(Header) + "accounts.nintendo.com\n");
    assert(one != 0 && one != all);
    const char first[] = "ACCOUNTS.NINTENDO.COM.";
    const char other[] = "api.accounts.nintendo.com";
    const char unapproved[] = "eshop.nintendo.net";
    assert(Matches(one, first, sizeof(first)));
    assert(!Matches(one, other, sizeof(other)));
    assert(!Matches(all, unapproved, sizeof(unapproved)));
    assert(!Matches(0, first, sizeof(first)));
    assert(!Matches(all, nullptr, 0));
    assert(Parse(complete + "accounts.nintendo.com\n") == 0);
    assert(Parse(complete + "*.nintendo.net\n") == 0);
    assert(Parse(complete + "roomcircuit.native\n") == 0);
    assert(Parse(complete + "%-lp1.baas.nintendo.com\n") == 0);
    assert(Parse(complete.substr(0, complete.size() - 1)) == 0);
    assert(Parse(complete + std::string(MaxBytes, 'x')) == 0);
    assert(Parse("RoomCircuitNativeDNS1 192.0.2.1\naccounts.nintendo.com\n") == 0);

    std::uint16_t port = 0;
    assert(ParseService("https", port) && port == 443);
    assert(ParseService("65535", port) && port == 65535);
    assert(!ParseService("65536", port));
    assert(!ParseService("443!", port));
    assert(!ParseService("http", port));
    assert(!ParseService("", port));
    static_assert(Endpoint == 0xcf93e931);

    std::string wire_hint("\xbe\xef\xca\xfe", 4);
    wire_hint.resize(33);
    wire_hint[11] = 2;
    wire_hint[15] = 1;
    wire_hint[19] = 6;
    AddrInfoHint hint{};
    assert(ParseAddrInfoHint(wire_hint, hint));
    assert(hint.flags == 0 && hint.family == 2 && hint.socket_type == 1 && hint.protocol == 6);
    assert(!ParseAddrInfoHint(wire_hint.substr(0, 32), hint));
    wire_hint[23] = 1;
    assert(!ParseAddrInfoHint(wire_hint, hint));
}
