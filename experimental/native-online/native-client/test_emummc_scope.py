#!/usr/bin/env python3
"""Run the patched Atmosphere selection predicate with a mocked SPL read."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


HEADER = Path("stratosphere/ams_mitm/source/fs_mitm/fs_mitm_service.hpp")
PATCH = Path(__file__).with_name("emummc-network-only.patch").resolve()


def function(source, opening):
    start = source.index(opening)
    depth = 0
    for pos in range(source.index("{", start), len(source)):
        char = source[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise ValueError(f"Open brace in {opening}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="unmodified Atmosphere 1.12.0 source checkout")
    parser.add_argument("--private-certstore", action="store_true", help="test the final private-storage variant")
    args = parser.parse_args()
    root = args.source.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="roomcircuit-emummc-scope-") as directory:
        build = Path(directory)
        (build / HEADER).parent.mkdir(parents=True)
        shutil.copyfile(root / HEADER, build / HEADER)
        subprocess.run(["git", "apply", str(PATCH)], cwd=build, check=True)
        if args.private_certstore:
            impl = HEADER.with_suffix(".cpp")
            shutil.copyfile(root / impl, build / impl)
            extra = PATCH.with_name("private-certstore.patch")
            subprocess.run(["git", "apply", str(extra)], cwd=build, check=True)
        header = (build / HEADER).read_text()
        program = function(header, "static constexpr ALWAYS_INLINE bool ShouldMitmProgramId(")
        emummc = function(header, "static bool IsPrivateNetworkEmummc(") if args.private_certstore else ""
        client = function(header, "static bool ShouldMitm(")
        unit = build / "scope.cpp"
        unit.write_text("\n".join((STUBS, program, emummc, client, TESTS)))
        executable = build / "scope"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: SSL emuMMC scope before/after HOME, failed and unknown config, and unchanged other clients")


STUBS = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>

#define ALWAYS_INLINE
using u64 = std::uint64_t;
namespace ncm {
    using ProgramId = unsigned;
    namespace SystemProgramId { enum { Ssl = 1, Ns, Settings, Sdb, Glue, Other }; }
    namespace SystemAppletId { enum { Qlaunch = 100, MaintenanceMenu }; }
    constexpr bool IsSystemProgramId(ProgramId id) { return id < 100; }
}
namespace sm { struct MitmProcessInfo { ncm::ProgramId program_id; }; }
namespace spl {
    enum class ConfigItem { ExosphereEmummcType };
    namespace smc {
        enum class Result { Success, Failure };
        static u64 mock_value = 0;
        static Result mock_result = Result::Success;
        static unsigned config_reads = 0;
        Result GetConfig(u64 *out, unsigned count, ConfigItem item) {
            assert(count == 1 && item == ConfigItem::ExosphereEmummcType);
            ++config_reads;
            *out = mock_value;
            return mock_result;
        }
    }
}
"""

TESTS = r"""
int main() {
    const sm::MitmProcessInfo ssl{ncm::SystemProgramId::Ssl};
    assert(!ShouldMitm(ssl));
    spl::smc::mock_value = 1;
    assert(ShouldMitm(ssl));
    spl::smc::mock_result = spl::smc::Result::Failure;
    assert(!ShouldMitm(ssl));
    spl::smc::mock_result = spl::smc::Result::Success;
    spl::smc::mock_value = 2;
    assert(!ShouldMitm(ssl));

    const unsigned before = spl::smc::config_reads;
    assert(!ShouldMitm({ncm::SystemProgramId::Other}));
    assert(ShouldMitm({ncm::SystemProgramId::Ns}));
    assert(ShouldMitm({ncm::SystemAppletId::Qlaunch}));
    assert(ShouldMitm({ncm::SystemProgramId::Other}));
    assert(spl::smc::config_reads == before);

    spl::smc::mock_value = 0;
    assert(!ShouldMitm(ssl));
    spl::smc::mock_value = 1;
    assert(ShouldMitm(ssl));
    spl::smc::mock_result = spl::smc::Result::Failure;
    assert(!ShouldMitm(ssl));
    spl::smc::mock_result = spl::smc::Result::Success;
    spl::smc::mock_value = 2;
    assert(!ShouldMitm(ssl));
}
"""


if __name__ == "__main__":
    main()
