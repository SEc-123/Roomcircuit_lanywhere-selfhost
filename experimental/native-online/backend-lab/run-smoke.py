#!/usr/bin/env python3
"""Run the isolated NEX integration test against a local account service."""

import argparse
import json
import os
import stat
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
VARIABLES = (
    "RC_IDENTITY_URL", "RC_IDENTITY_KEY", "RC_HOST_TOKEN", "RC_GUEST_TOKEN",
    "RC_HOST_PID", "RC_GUEST_PID",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--source", type=Path, help="Existing checkout or archive of openpak/switch-nex v0.3.45")
    inputs.add_argument("--binary", type=Path, help="Previously compiled integration executable")
    commands = parser.add_mutually_exclusive_group(required=True)
    commands.add_argument("--credentials", type=Path, help="Owner-only JSON with account lab credentials")
    commands.add_argument("--compile-only", action="store_true")
    commands.add_argument("--linux-binary", type=Path, help="Compile the test executable for Linux x86-64")
    args = parser.parse_args()
    environment = os.environ.copy()
    environment.update(GOWORK="off", GOTOOLCHAIN="local", GOPROXY="off")
    for variable in VARIABLES:
        environment.pop(variable, None)
    if args.credentials:
        with args.credentials.open() as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
                parser.error("credentials must be an owner-only regular JSON file")
            stored = json.load(stream)
        if not isinstance(stored, dict) or any(not isinstance(stored.get(name), (str, int)) for name in VARIABLES):
            parser.error("credentials do not match the account lab contract")
        environment.update({name: str(stored[name]) for name in VARIABLES})
    pattern = "^TestRoomCircuitAuthenticatedRoom$"
    if args.binary:
        if not args.credentials:
            parser.error("--binary requires --credentials")
        subprocess.run([str(args.binary.resolve()), "-test.v", "-test.timeout=35s", "-test.run", pattern],
                       env=environment, check=True)
        return
    source = args.source.resolve()
    if not (source / "cmd" / "switch-nexd" / "main.go").is_file():
        parser.error("--source must contain the OpenPak Switch NEX command")
    with tempfile.TemporaryDirectory(prefix=".smoke-", dir=ROOT) as scratch:
        overlay = Path(scratch) / "overlay.json"
        virtual_test = source / "cmd" / "switch-nexd" / "roomcircuit_authenticated_room_test.go"
        overlay.write_text(json.dumps({"Replace": {str(virtual_test): str(ROOT / "authenticated_room_test.go")}}))
        command = ["go", "test", f"-overlay={overlay}"]
        if args.linux_binary:
            environment.update(GOOS="linux", GOARCH="amd64", CGO_ENABLED="0")
            command.extend(["-c", "-o", str(args.linux_binary.resolve()), "./cmd/switch-nexd"])
        else:
            command.extend(["-count=1", "-v", "-timeout=35s", "./cmd/switch-nexd",
                            "-list" if args.compile_only else "-run", pattern])
        subprocess.run(command, cwd=source, env=environment, check=True)


if __name__ == "__main__":
    main()
