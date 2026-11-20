#!/usr/bin/env python3
"""Install and inspect a Switch LAN Play node on a Debian or Ubuntu VPS."""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
from urllib.request import urlopen

IMAGE = "spacemeowx2/switch-lan-play@sha256:853d10e28ce13476e9cdcd7e545e84cda69a6971528c2fbd7be38dd23a97b114"
CONTAINER = "roomcircuit-lanplay-node"
MANAGED_LABEL = "org.roomcircuit.managed-by"
MANAGED_VALUE = "roomcircuit-lanplay-node-setup"
DEFAULT_PORT = 11451
REGIONS = ("unknown", "asia", "europe", "americas", "oceania", "africa")


def run(command, check=True, capture=False):
    return subprocess.run(
        command,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def normalize_host(value):
    host = value.strip().rstrip(".")
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        address = None
    if address is not None:
        if not isinstance(address, ipaddress.IPv4Address) or not address.is_global:
            raise ValueError("Use a public IPv4 address or a DNS hostname.")
        return str(address)

    labels = host.lower().split(".")
    if not host or len(host) > 253 or len(labels) < 2:
        raise ValueError("Use a public IPv4 address or a DNS hostname.")
    for label in labels:
        if (
            not label
            or len(label) > 63
            or label.startswith("-")
            or label.endswith("-")
            or not re.fullmatch(r"[a-z0-9-]+", label)
        ):
            raise ValueError("Use a public IPv4 address or a DNS hostname.")
    return host.lower()


def validate_name(value):
    name = value.strip()
    encoded = name.encode("utf-8")
    if not name or len(encoded) >= 48 or any(ord(char) < 0x20 or ord(char) == 0x7F for char in name):
        raise ValueError("The display name must be 1–47 UTF-8 bytes and contain no control characters.")
    return name


def prompt_value(value, prompt, default=None):
    if value:
        return value
    if not sys.stdin.isatty():
        if default is not None:
            return default
        raise RuntimeError("Provide the missing value with a command-line option.")
    suffix = " [{}]".format(default) if default is not None else ""
    response = input(prompt + suffix + ": ").strip()
    return response or default


def check_port_available(port):
    for kind, address, label in (
        (socket.SOCK_DGRAM, "0.0.0.0", "UDP"),
        (socket.SOCK_STREAM, "127.0.0.1", "TCP monitor"),
    ):
        probe = socket.socket(socket.AF_INET, kind)
        try:
            probe.bind((address, port))
        except OSError as error:
            raise RuntimeError("{} port {} is already in use.".format(label, port)) from error
        finally:
            probe.close()


def read_distribution():
    try:
        lines = Path("/etc/os-release").read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RuntimeError("Cannot read /etc/os-release.") from error
    values = {}
    for line in lines:
        key, separator, value = line.partition("=")
        if separator:
            values[key] = value.strip().strip('"')
    return values.get("ID", "").lower()


def ensure_docker():
    if not shutil.which("docker"):
        if read_distribution() not in ("debian", "ubuntu") or not shutil.which("apt-get"):
            raise RuntimeError("Automatic Docker installation supports Debian and Ubuntu. Install Docker manually on this system.")
        print("Installing Docker from the Debian/Ubuntu package repository...")
        run(["apt-get", "update"])
        run(["apt-get", "install", "-y", "docker.io"])

    if shutil.which("systemctl"):
        run(["systemctl", "enable", "--now", "docker"])
    elif shutil.which("service"):
        run(["service", "docker", "start"])

    result = run(["docker", "info"], check=False, capture=True)
    if result.returncode:
        raise RuntimeError("Docker is installed but not running. Start the Docker service and rerun this script.")


def allow_ufw_udp(port):
    ufw = shutil.which("ufw")
    if not ufw:
        print("No UFW rule was changed. If a host firewall is active, allow inbound UDP {}.".format(port))
        return
    status = run([ufw, "status"], check=False, capture=True)
    output = (status.stdout or "") + (status.stderr or "")
    if status.returncode or "Status: active" not in output:
        print("UFW is not active; it was left unchanged.")
        return
    if "{}/udp".format(port) not in output:
        rule = run([ufw, "allow", "{}/udp".format(port)], check=False, capture=True)
        if rule.returncode:
            print("Could not update UFW. Allow inbound UDP {} in your host firewall.".format(port))
            return
    print("UFW allows inbound UDP {}.".format(port))


def online_count(port):
    with urlopen("http://127.0.0.1:{}/info".format(port), timeout=3) as response:
        data = json.load(response)
    count = data.get("online")
    if not isinstance(count, int) or count < 0:
        raise ValueError("The LAN Play monitor returned an unexpected response.")
    return count


def show_status(port):
    try:
        print("Connected LAN Play clients: {}".format(online_count(port)))
        print("The monitor reports a client count, not game rooms or player identities.")
        return 0
    except Exception as error:
        print("Could not read the local status monitor: {}".format(error), file=sys.stderr)
        print("Run this command on the VPS and check that the node is running.", file=sys.stderr)
        return 1


def inspect_existing_container():
    result = run(["docker", "container", "inspect", CONTAINER], check=False, capture=True)
    if result.returncode:
        return False
    try:
        details = json.loads(result.stdout)[0]
        labels = details.get("Config", {}).get("Labels") or {}
    except (IndexError, json.JSONDecodeError):
        labels = {}
    if labels.get(MANAGED_LABEL) != MANAGED_VALUE:
        raise RuntimeError("Container '{}' already exists and was left unchanged.".format(CONTAINER))
    return True


def install_node(args):
    if os.geteuid() != 0:
        raise RuntimeError("Run installation with sudo, for example: sudo python3 setup_lanplay_node.py")
    if not 1 <= args.port <= 65535:
        raise RuntimeError("--port must be between 1 and 65535.")

    host = normalize_host(prompt_value(args.host, "Public IPv4 or hostname for this node"))
    name = validate_name(prompt_value(args.name, "Display name", "My LAN Play node"))
    region = prompt_value(
        args.region,
        "Region: unknown, asia, europe, americas, oceania or africa",
        "unknown",
    ).lower()
    if region not in REGIONS:
        raise RuntimeError("--region must be one of: {}.".format(", ".join(REGIONS)))

    ensure_docker()
    if inspect_existing_container():
        print("This script's node container already exists; it was left unchanged.")
        print("Use --status with the port it was installed on to check its client count.")
        return 0

    check_port_available(args.port)
    print("Downloading the official LAN Play server image...")
    run(["docker", "pull", IMAGE])
    run([
        "docker", "run", "--detach",
        "--name", CONTAINER,
        "--label", "{}={}".format(MANAGED_LABEL, MANAGED_VALUE),
        "--restart", "unless-stopped",
        "--publish", "{}:11451/udp".format(args.port),
        "--publish", "127.0.0.1:{}:11451/tcp".format(args.port),
        IMAGE,
    ])
    allow_ufw_udp(args.port)

    node_config = (
        "[node]\n"
        "name={}\n"
        "host={}\n"
        "port={}\n"
        "region={}\n"
    ).format(name, host, args.port, region)
    snippet = Path.cwd() / "roomcircuit-node.ini"
    try:
        snippet.write_text(node_config, encoding="utf-8")
    except OSError as error:
        print("Could not save roomcircuit-node.ini: {}".format(error), file=sys.stderr)

    print("\nLAN Play node started.")
    print("Public endpoint: {}:{}".format(host, args.port))
    print("Local-only monitor: http://127.0.0.1:{}/info".format(args.port))
    print("Allow inbound UDP {} in your VPS provider's cloud firewall/security group.".format(args.port))
    print("\nAdd this block to config/RoomCircuit/nodes.ini on each Switch:")
    print(node_config.rstrip())
    print("\nA copy was saved to {}.".format(snippet))
    print("Check the connected-client count on this VPS with:")
    print("sudo python3 setup_lanplay_node.py --status --port {}".format(args.port))
    print("RoomCircuit has no per-node login fields; anyone who can reach this UDP port can use the relay.")

    for _ in range(8):
        try:
            print("Monitor check passed. Connected clients: {}".format(online_count(args.port)))
            break
        except Exception:
            time.sleep(1)
    else:
        print("The server started, but its monitor has not answered yet. Check: docker logs {}".format(CONTAINER))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="public IPv4 or DNS hostname to place in nodes.ini")
    parser.add_argument("--name", help="display name in the RoomCircuit node list")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="public UDP port (default: 11451)")
    parser.add_argument("--region", choices=REGIONS, help="node-list region label (default: unknown)")
    parser.add_argument("--status", action="store_true", help="show the local connected-client count")
    args = parser.parse_args()

    if args.status:
        return show_status(args.port)
    try:
        return install_node(args)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print("Setup failed: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
