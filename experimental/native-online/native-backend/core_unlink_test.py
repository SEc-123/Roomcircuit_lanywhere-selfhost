#!/usr/bin/env python3
"""Test same-subject reactivation against the real disposable account core."""

import argparse
import hashlib
import http.cookies
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import ssl
import subprocess
import tempfile
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPSHandler, ProxyHandler, Request, build_opener

from durable_test import ports
import smoke


APPLICATION = "01006fe013472000"
NAMESPACE = "roomcircuit-lab:"
IDENTITY = "/internal/switch/identity"
CHECK = "/internal/grants/check"


class Broker:
    def __init__(self, executable, environment, public, private):
        self.executable = executable
        self.environment = environment
        self.public = public
        self.private = private
        self.key = {"X-Internal-Key": environment["RC_DEVICE_GAME_KEY"]}
        self.process = None
        ca = ssl.create_default_context(cafile=str(smoke.ROOT / ".runtime/ca.pem"))
        self.client = build_opener(ProxyHandler({}), HTTPSHandler(context=ca))

    def start(self):
        self.process = subprocess.Popen([str(self.executable)], env=self.environment)
        smoke.account.ready(self.private, "/healthz")

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=6)

    def call(self, route, data=None, *, expected=(200,), private=False, form=None):
        origin = self.private if private else self.public
        headers = {"Content-Type": "application/json", **(self.key if private else {})}
        if form is not None:
            headers.update({"Cookie": form, "Origin": self.public, "Content-Type": "application/x-www-form-urlencoded"})
        contents = (urlencode(data) if form is not None else json.dumps(data)).encode() if data is not None else None
        request = Request(origin + route, contents, headers)
        try:
            result = self.client.open(request, timeout=10)
        except HTTPError as error:
            result = error
        with result:
            if result.status not in expected:
                safe_route = route.split("?", 1)[0]
                raise AssertionError(f"{safe_route} returned HTTP {result.status}, expected one of {expected}")
            body = result.read(16385)
            if len(body) > 16384:
                raise AssertionError("broker test response exceeds the expected size")
            if "application/json" in result.headers.get("Content-Type", ""):
                body = json.loads(body)
            return body, result.headers

    def approve(self, credentials, device):
        item, _ = self.call("/v1/device/authorize", {
            "device_key": device, "scope": "roomcircuit-game", "application_id": APPLICATION,
        }, expected=(201,))
        page, headers = self.call("/device?" + urlencode({"user_code": item["user_code"]}))
        csrf = re.search(rb'name="csrf_token" value="([a-f0-9]+)"', page)
        if csrf is None:
            raise AssertionError("browser approval form is missing the CSRF field")
        cookie = http.cookies.SimpleCookie(headers["Set-Cookie"])["rc_device_form"].value
        approved, _ = self.call("/device", {**credentials, "user_code": item["user_code"],
            "csrf_token": csrf[1].decode(), "decision": "approve"}, form="rc_device_form=" + cookie)
        if approved["status"] != "approved":
            raise AssertionError("browser did not approve the synthetic device")
        return item["device_code"]

    def tokens(self, code, expected=(200,)):
        return self.call("/v1/device/token", {"device_code": code}, expected=expected)[0]

    def identity(self, token, expected=(200,)):
        return self.call(IDENTITY, {"id_token": token, "application_id": APPLICATION}, expected=expected, private=True)[0]

    def grant(self, grant_id, expected=(200,)):
        return self.call(CHECK, {"grant_id": grant_id, "application_id": APPLICATION}, expected=expected, private=True)[0]

    def denied(self, tokens, identity):
        self.grant(identity["grant_id"], expected=(403, 404))
        self.identity(tokens["id_token"], expected=(401, 403, 404))
        self.call("/v1/device/refresh", {"refresh_token": tokens["refresh_token"],
            "application_id": APPLICATION}, expected=(401, 403))

    def processed(self, version):
        state = Path(self.environment["RC_DEVICE_GRANT_STATE"])
        cursor = json.loads(state.read_text()).get("events_cursor")
        if not isinstance(cursor, int) or cursor < version:
            raise AssertionError("grant and invalidation cursor were not persisted together")


def relink(helper, address, core_key, owner, device):
    subject = hashlib.sha256(NAMESPACE.encode() + bytes.fromhex(device)).hexdigest()
    environment = {**os.environ, "RC_TEST_CORE_ADDRESS": address, "RC_TEST_CORE_KEY": core_key}
    result = subprocess.run([str(helper)], input=json.dumps({"account": owner, "subject": subject}),
                            env=environment, text=True, stdout=subprocess.PIPE, timeout=16, check=True)
    version = json.loads(result.stdout)["unlink_version"]
    if not isinstance(version, int) or version <= 0:
        raise AssertionError("the actual core unlink event has no committed cursor")
    return version


def verify(broker, helper, core_address, core_key):
    credentials_a, owner_a = smoke.create_account("UnlinkHost", "US")
    credentials_b, owner_b = smoke.create_account("UnlinkGuest", "GB")
    device_a, device_b = secrets.token_hex(32), secrets.token_hex(32)
    tokens_a = broker.tokens(broker.approve(credentials_a, device_a))
    tokens_b = broker.tokens(broker.approve(credentials_b, device_b))
    initial_a = broker.identity(tokens_a["id_token"])
    initial_b = broker.identity(tokens_b["id_token"])
    assert initial_a["account_id"] == owner_a and initial_b["account_id"] == owner_b
    assert initial_a["pid"] != initial_b["pid"]

    broker.stop()
    offline_version = relink(helper, core_address, core_key, owner_a, device_a)
    broker.start()
    broker.denied(tokens_a, initial_a)
    broker.processed(offline_version)
    assert broker.grant(initial_b["grant_id"]) == initial_b

    fresh_a = broker.tokens(broker.approve(credentials_a, device_a))
    current_a = broker.identity(fresh_a["id_token"])
    assert current_a["grant_id"] != initial_a["grant_id"]
    assert current_a["pid"] == initial_a["pid"] and current_a["account_id"] == owner_a
    broker.denied(tokens_a, initial_a)
    broker.stop()
    broker.start()
    assert broker.grant(current_a["grant_id"]) == current_a
    assert broker.identity(fresh_a["id_token"]) == current_a
    broker.denied(tokens_a, initial_a)

    stale_code = broker.approve(credentials_b, device_b)
    online_version = relink(helper, core_address, core_key, owner_b, device_b)
    assert online_version > offline_version
    # Poll before any other broker request can perform the event refresh.
    assert broker.tokens(stale_code, expected=(403,))["error"] == "device_link_revoked"
    broker.denied(tokens_b, initial_b)
    broker.processed(online_version)
    assert broker.identity(fresh_a["id_token"]) == current_a

    fresh_b = broker.tokens(broker.approve(credentials_b, device_b))
    current_b = broker.identity(fresh_b["id_token"])
    assert current_b["grant_id"] != initial_b["grant_id"]
    assert current_b["pid"] == initial_b["pid"] and current_b["account_id"] == owner_b
    return {
        "postgresql_core_unlink_events_committed": True,
        "offline_unlink_and_reactivation_permanently_revoke_old_grant": True,
        "new_explicit_approval_same_owner_and_subject_preserves_pid": True,
        "new_grant_survives_next_restart_and_event_replay": True,
        "core_event_cursor_persisted": True,
        "stale_approved_challenge_cannot_regrant_after_online_unlink": True,
        "other_account_preserved": True,
    }


def main():
    if not __debug__:
        raise RuntimeError("Run this test with assertions enabled")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broker-binary", type=Path, help="Existing host-native broker to test")
    parser.add_argument("--receipt", type=Path, default=smoke.ROOT / "core-unlink-receipt.json")
    args = parser.parse_args()
    root, upstream = smoke.ROOT, smoke.ROOT.parent / "account-lab"
    with tempfile.TemporaryDirectory(prefix="roomcircuit-core-unlink-") as temporary:
        workspace = Path(temporary)
        account = workspace / "account-lab"
        (account / ".bin").mkdir(parents=True)
        for path in ("lab.sh", "compose.yaml", ".bin/account", ".bin/identity"):
            shutil.copy2(upstream / path, account / path)
        account_port, identity_port, grpc_port, public_port, private_port = ports(5)
        project = "roomcircuit-unlink-proof-" + secrets.token_hex(4)
        compose = {**os.environ, "COMPOSE_PROJECT_NAME": project, "RC_ACCOUNT_PORT": str(account_port),
                   "RC_IDENTITY_PORT": str(identity_port), "RC_ACCOUNT_GRPC_PORT": str(grpc_port)}
        public, private = f"https://127.0.0.1:{public_port}", f"http://127.0.0.1:{private_port}"
        smoke.account.ACCOUNT, smoke.account.IDENTITY = f"http://127.0.0.1:{account_port}", f"http://127.0.0.1:{identity_port}"
        core_address = f"127.0.0.1:{grpc_port}"
        executable = args.broker_binary.resolve() if args.broker_binary else workspace / "native-backend"
        helper = workspace / "core-unlink-driver"
        if not args.broker_binary:
            subprocess.run(["go", "build", "-mod=readonly", "-trimpath", "-o", str(executable), "."], cwd=root, check=True)
        subprocess.run(["go", "build", "-mod=readonly", "-trimpath", "-o", str(helper), "./testdata/core-unlink"], cwd=root, check=True)
        dotenv = account / ".runtime/lab.env"
        broker = None
        try:
            subprocess.run(["sh", "lab.sh", "up"], cwd=account, env=compose, check=True)
            smoke.account.ready(smoke.account.ACCOUNT, "/healthz")
            keys = dict(line.split("=", 1) for line in dotenv.read_text().splitlines())
            runtime = root / ".runtime"
            environment = {**os.environ, "RC_DEVICE_ISSUER": public, "RC_DEVICE_LISTEN": f"127.0.0.1:{public_port}",
                "RC_DEVICE_INTERNAL_LISTEN": f"127.0.0.1:{private_port}", "RC_ACCOUNT_URL": smoke.account.ACCOUNT,
                "RC_LINKS_URL": smoke.account.IDENTITY, "RC_LINKS_KEY": keys["RC_IDENTITY_KEY"],
                "RC_ACCOUNT_CORE_GRPC": core_address, "RC_ACCOUNT_CORE_KEY": keys["RC_CORE_KEY"],
                "RC_DEVICE_GAME_KEY": secrets.token_hex(32), "RC_DEVICE_CERT": str(runtime / "server.pem"),
                "RC_DEVICE_TLS_KEY": str(runtime / "server-key.pem"), "RC_DEVICE_SIGNING_KEY": str(runtime / "issuer-key.pem"),
                "RC_DEVICE_GRANT_STATE": str(workspace / "approved-grants.json"), "RC_DEVICE_APPLICATIONS": APPLICATION}
            broker = Broker(executable, environment, public, private)
            broker.start()
            receipt = verify(broker, helper, core_address, keys["RC_CORE_KEY"])
            args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
            print("Real PostgreSQL core unlink, offline replay, stale approval refusal and explicit relink passed.")
        finally:
            if broker is not None:
                broker.stop()
            if dotenv.is_file():
                subprocess.run(["docker", "compose", "-p", project, "--env-file", ".runtime/lab.env", "down", "--volumes"],
                               cwd=account, env=compose, check=True)


if __name__ == "__main__":
    main()
