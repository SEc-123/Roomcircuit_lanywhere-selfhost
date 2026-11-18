#!/usr/bin/env python3
"""Type-check native views and exercise the Swift client against an isolated local relay."""

from pathlib import Path
import os
import socket
import subprocess
import tempfile
import time
import urllib.request
import urllib.error

HERE = Path(__file__).resolve().parent
APPLE = HERE.parent
ROOT = APPLE.parents[1]
SOURCES = sorted(APPLE.joinpath("Sources").glob("*.swift"))


def run(*command):
    subprocess.run(command, cwd=ROOT, check=True)


def unused_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_relay(relay, log, origin):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if relay.poll() is not None:
            raise RuntimeError(log.read_text())
        try:
            with urllib.request.urlopen(f"{origin}/api/appointments/options", timeout=1) as response:
                if response.status == 200 and b'"nativeApi":1' in response.read():
                    return
        except (TimeoutError, urllib.error.URLError):
            time.sleep(0.15)
    raise RuntimeError(f"Native relay option endpoint did not start:\n{log.read_text()}")


def main():
    run("swiftc", "-swift-version", "6", "-typecheck", *SOURCES)
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    run("xcrun", "--sdk", "iphoneos", "swiftc", "-swift-version", "6", "-sdk", sdk,
        "-target", "arm64-apple-ios26.0", "-typecheck", *SOURCES)
    print("SwiftUI modules type-checked for macOS host and arm64 iOS.")

    with tempfile.TemporaryDirectory(prefix="roomcircuit-swift-test-") as sandbox:
        scratch = Path(sandbox)
        binary = scratch / "probe"
        run("swiftc", "-swift-version", "6", APPLE / "Sources" / "RoomCircuitService.swift",
            HERE / "probe.swift", "-o", binary)
        web = unused_port(socket.SOCK_STREAM)
        lanplay = unused_port(socket.SOCK_DGRAM)
        origin = f"http://127.0.0.1:{web}"
        state = scratch / "state"
        environment = os.environ.copy()
        environment.update({
            "LANYWHERE_STATE_DIR": str(state), "LANYWHERE_CONTROL_BIND_ADDRESS": "127.0.0.1",
            "LANYWHERE_UDP_BIND_ADDRESS": "127.0.0.1",
            "LANYWHERE_LANPLAY_BIND_ADDRESS": "127.0.0.1", "LANYWHERE_LANPLAY_PORT": str(lanplay),
            "LANYWHERE_STATS_PORT": "0", "LANYWHERE_PUBLIC_PORT": "0", "LANYWHERE_OPS_PORT": "0",
            "ROOMCIRCUIT_EDEN_PORT": "0", "ROOMCIRCUIT_YUZU_PORT": "0", "ROOMCIRCUIT_RYULDN_PORT": "0",
            "ROOMCIRCUIT_WEB_PORT": str(web), "ROOMCIRCUIT_WEB_BIND": "127.0.0.1",
            "ROOMCIRCUIT_WEB_ORIGIN": origin, "ROOMCIRCUIT_WEB_RELAY_HOST": "127.0.0.1",
        })
        environment.pop("LANYWHERE_LANPLAY_REALM", None)
        log = scratch / "relay.log"
        with log.open("w") as output:
            with subprocess.Popen(["node", "server/relay/server.js", "--port", "0"], cwd=ROOT,
                                  env=environment, stdout=output, stderr=subprocess.STDOUT) as server:
                try:
                    wait_for_relay(server, log, origin)
                    run(binary, origin)
                finally:
                    server.terminate()
                    try:
                        server.wait(5)
                    except subprocess.TimeoutExpired:
                        server.kill()


if __name__ == "__main__":
    main()
