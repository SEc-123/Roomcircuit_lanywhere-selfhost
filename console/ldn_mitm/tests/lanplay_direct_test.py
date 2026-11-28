#!/usr/bin/env python3
"""Build the production direct transport and exercise independent wire peers."""
import os
import json
import re
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NativeLanPlayTransport(unittest.TestCase):
    def test_switch_virtual_address_uses_mounted_filesystem(self):
        source = (ROOT / "console/ldn_mitm/source/lanplay_direct.cpp").read_text()
        def function(signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth = 1
            end = opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]
        address = function("uint32_t VirtualAddress(bool renew) {")
        for forbidden in ["fopen", "fwrite", "fread", "mkdir", "fsync"]:
            self.assertNotIn(forbidden, address)
        main = (ROOT / "console/ldn_mitm/source/ldnmitm_main.cpp").read_text()
        self.assertIn('fs::MountSdCard("sdmc")', main)
        harness = (ROOT / "console/ldn_mitm/tests/lanplay_address_store_harness.cpp.in").read_text()
        harness = harness.replace("@ADDRESS@", address).replace("@VIRTUAL@", function("bool Virtual(uint32_t ip) {"))
        harness = harness.replace("@CODEC@", str(ROOT / "console/ldn_mitm/source/lanplay_codec.hpp"))
        with tempfile.TemporaryDirectory(prefix="lanywhere-switch-storage-") as temp:
            cpp = Path(temp) / "address.cpp"
            executable = Path(temp) / "address"
            cpp.write_text(harness)
            subprocess.run(shlex.split(os.environ.get("CXX", "c++")) +
                ["-std=c++17", "-D__SWITCH__", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "common"), str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            subprocess.run([str(executable), "v1"], check=True)
            subprocess.run([str(executable), "current"], check=True)

    def test_switch_thread_priority_uses_stratosphere_conversion(self):
        implementation = ROOT / "console/Atmosphere-libs/libstratosphere/source/os/impl"
        header = (implementation / "os_thread_manager_impl.os.horizon.hpp").read_text()
        source = (implementation / "os_thread_manager_impl.os.horizon.cpp").read_text()
        offset = int(re.search(r"UserThreadPriorityOffset\s*=\s*(\d+)", header)[1])
        self.assertIn("user_priority + UserThreadPriorityOffset", source)
        self.assertIn("ConvertToHorizonPriority(thread->base_priority)", source)
        metadata = json.loads((ROOT / "console/ldn_mitm/res/app.json").read_text())
        flags = next(item["value"] for item in metadata["kernel_capabilities"] if item["type"] == "kernel_flags")
        bounds = sorted([flags["highest_thread_priority"], flags["lowest_thread_priority"]])
        checked = 0
        for filename in ["lanplay_direct.cpp", "lanywhere_relay.cpp"]:
            code = (ROOT / "console/ldn_mitm/source" / filename).read_text()
            for match in re.finditer(r"(?:ams::)?os::CreateThread\(", code):
                # Accept both assigned results and calls nested inside R_FAILED.
                # The actual argument list can itself contain sizeof/addressof.
                end, depth = match.end(), 1
                while end < len(code) and depth:
                    depth += (code[end] == "(") - (code[end] == ")")
                    end += 1
                self.assertEqual(depth, 0, f"unclosed CreateThread in {filename}")
                arguments = code[match.end():end - 1]
                literal = re.search(r",\s*(0x[0-9a-fA-F]+|[0-9]+)\s*$", arguments)
                self.assertIsNotNone(literal, f"priority must be reviewed in {filename}: {arguments}")
                kernel = int(literal[1], 0) + offset
                self.assertTrue(bounds[0] <= kernel <= bounds[1], f"{filename}: kernel priority {kernel} outside NPDM {bounds}")
                checked += 1
        self.assertGreaterEqual(checked, 3)

    def test_direct_wire_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="lanywhere-native-lanplay-") as temp:
            build = Path(temp) / "lwip"
            subprocess.run(
                ["make", "-s", "-C", str(ROOT / "console/lanplay-lwip"), "-j4", f"BUILD={build}"],
                check=True,
            )
            driver = Path(temp) / "native-lanplay-driver"
            subprocess.run(
                shlex.split(os.environ.get("CXX", "c++"))
                + ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
                   f"-I{ROOT / 'console/lanplay-lwip/include'}",
                   str(ROOT / "console/ldn_mitm/tests/lanplay_direct_driver.cpp"),
                   str(ROOT / "console/ldn_mitm/source/lanplay_direct.cpp"),
                   str(build / "liblanplay_lwip.a"), "-o", str(driver)],
                check=True,
            )
            subprocess.run(
                [str(driver), "hostidentity", "12345"], check=True, timeout=5,
            )
            subprocess.run(
                ["node", str(ROOT / "console/ldn_mitm/tests/lanplay_direct_test.cjs"), str(driver)],
                check=True, timeout=130,
            )
            for fixture in ["lanplay_protocol_lock", "lanplay_fragment_queue", "lanplay_multi"]:
                executable = Path(temp) / fixture
                subprocess.run(
                    shlex.split(os.environ.get("CXX", "c++"))
                    + ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
                       f"-I{ROOT / 'console/lanplay-lwip/include'}",
                       str(ROOT / f"console/ldn_mitm/tests/{fixture}_driver.cpp"),
                       str(ROOT / "console/ldn_mitm/source/lanplay_direct.cpp"),
                       str(build / "liblanplay_lwip.a"), "-o", str(executable)],
                    check=True,
                )
                subprocess.run(
                    ["node", str(ROOT / f"console/ldn_mitm/tests/{fixture}_test.cjs"), str(executable)],
                    check=True, timeout=60,
                )
                if fixture == "lanplay_multi":
                    subprocess.run(
                        ["node", str(ROOT / "console/ldn_mitm/tests/lanplay_stale_heartbeat_test.cjs"), str(executable)],
                        check=True, timeout=30,
                    )


if __name__ == "__main__":
    unittest.main()
