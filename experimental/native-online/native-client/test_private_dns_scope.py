#!/usr/bin/env python3
"""Exercise the patched loader with mock SD reads and stock resolver rules."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from test_emummc_scope import function

HERE = Path(__file__).resolve().parent
DNS = Path("stratosphere/ams_mitm/source/dns_mitm")
FILES = ("dnsmitm_host_redirection.cpp", "dnsmitm_host_redirection.hpp", "dnsmitm_resolver_impl.cpp")

STUBS = r"""
#include "private_dns_profile.hpp"
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
using s64 = std::int64_t;
using u64 = std::uint64_t;
namespace ams::socket { using InAddrT = std::uint32_t; }
namespace ams::fs { constexpr int OpenMode_Read = 1; }
using namespace ams;
struct FsFile {};
static unsigned opens = 0, closes = 0;
static bool complete_read = true, open_fails = false;
static std::string contents;
constexpr int FsReadOption_None = 0;
static int fsFileGetSize(FsFile *, s64 *length) { *length = contents.size(); return 0; }
static int fsFileRead(FsFile *, s64 offset, void *out, s64 len, int option, u64 *read) {
    assert(offset == 0 && option == FsReadOption_None && len > 0);
    std::memcpy(out, contents.data(), len);
    *read = len - !complete_read;
    return 0;
}
static void fsFileClose(FsFile *) { ++closes; }
namespace mitm::fs {
    int OpenAtmosphereSdFile(FsFile *, const char *path, unsigned mode) {
        assert(std::string_view(path) == "/roomcircuit-native/DNS/profile.txt" && mode == ams::fs::OpenMode_Read);
        ++opens;
        return open_fails;
    }
}
namespace hos {
    enum Version { Version_19_0_1, Other };
    static Version current = Version_19_0_1;
    Version GetVersion() { return current; }
}
namespace spl {
    enum class ConfigItem { ExosphereEmummcType };
    namespace smc {
        enum class Result { Success, Failure };
        static u64 mode = 0;
        static Result status = Result::Success;
        Result GetConfig(u64 *out, std::size_t count, ConfigItem item) {
            assert(count == 1 && item == ConfigItem::ExosphereEmummcType);
            *out = mode;
            return status;
        }
    }
}
template<typename Fn> struct ScopeExit { Fn free; ~ScopeExit() { free(); } };
struct Exit {
    template<typename Fn> auto operator+(Fn free) { return ScopeExit<Fn>{free}; }
};
#define ON_SCOPE_EXIT auto guard = Exit{} + [&]()
#define R_FAILED(expr) ((expr) != 0)
static std::mutex g_redirection_lock;
static std::vector<std::pair<std::string, ams::socket::InAddrT>> g_redirection_list;
static roomcircuit::private_dns::Profile g_private_dns_profile = 0;
"""

TESTS = r"""
int main() {
    using namespace roomcircuit::private_dns;
    const char alias[] = "accounts.nintendo.com", blocked[] = "eshop.nintendo.net", unrelated[] = "example.org";
    contents = std::string(Header) + alias + "\n";
    assert(LoadPrivateDnsProfile() == 0);
    spl::smc::mode = 2;
    assert(LoadPrivateDnsProfile() == 0);
    spl::smc::mode = 1;
    spl::smc::status = spl::smc::Result::Failure;
    assert(LoadPrivateDnsProfile() == 0);
    spl::smc::status = spl::smc::Result::Success;
    hos::current = hos::Other;
    assert(LoadPrivateDnsProfile() == 0 && opens == 0);
    hos::current = hos::Version_19_0_1;
    open_fails = true;
    assert(LoadPrivateDnsProfile() == 0 && opens == 1 && closes == 0);
    open_fails = false;
    complete_read = false;
    assert(LoadPrivateDnsProfile() == 0 && closes == 1);
    complete_read = true;
    g_private_dns_profile = LoadPrivateDnsProfile();
    assert(g_private_dns_profile != 0 && closes == 2);
    g_redirection_list.emplace_back("*nintendo*", 0x0100007f);
    std::uint32_t address = 0;
    bool matched = false;
    assert(GetRedirectedHostByName(&address, &matched, alias, sizeof(alias)) && matched && address == Endpoint);
    assert(GetRedirectedHostByName(&address, &matched, blocked, sizeof(blocked)) && !matched && address == 0x0100007f);
    assert(!GetRedirectedHostByName(&address, &matched, unrelated, sizeof(unrelated)) && !matched);
    contents += "unsupported.example.org\n";
    g_private_dns_profile = LoadPrivateDnsProfile();
    assert(g_private_dns_profile == 0 && closes == 3);
    assert(GetRedirectedHostByName(&address, &matched, alias, sizeof(alias)) && !matched && address == 0x0100007f);
}
"""

with tempfile.TemporaryDirectory(prefix="roomcircuit-dns-scope-") as temp:
    root = Path(sys.argv[1]).resolve(strict=True)
    build = Path(temp)
    (build / DNS).mkdir(parents=True)
    for name in FILES:
        shutil.copyfile(root / DNS / name, build / DNS / name)
    subprocess.run(["git", "apply", str(HERE / "private-dns-profile.patch")], cwd=build, check=True)
    content = (build / DNS / FILES[0]).read_text()
    selected = [function(content, signature) for signature in [
        "constexpr int wildcardcmp", "roomcircuit::private_dns::Profile LoadPrivateDnsProfile",
        "bool GetRedirectedHostByName",
    ]]
    unit = build / "scope.cpp"
    unit.write_text("\n".join((STUBS, *selected, TESTS)))
    executable = build / "scope"
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(HERE),
                    str(unit), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("PASS private DNS scope, read-only profile and ordinary block fallback")
