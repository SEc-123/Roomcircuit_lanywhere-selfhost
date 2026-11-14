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
            for match in re.finditer(r"(?:ams::)?os::CreateThread\((.*?)\)\)", code, re.S):
                literal = re.search(r",\s*(0x[0-9a-fA-F]+|[0-9]+)\s*$", match[1])
                self.assertIsNotNone(literal, f"priority must be reviewed in {filename}: {match[0]}")
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
                ["node", str(ROOT / "console/ldn_mitm/tests/lanplay_direct_test.cjs"), str(driver)],
                check=True, timeout=130,
            )
            for fixture in ["lanplay_protocol_lock", "lanplay_fragment_queue"]:
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


if __name__ == "__main__":
    unittest.main()
