#!/usr/bin/env python3
"""Exercise verified browser grants through the independent Switch NEX test realm."""

import argparse
import json
import os
import secrets
import ssl
import stat
import subprocess
import tempfile
import time
from html.parser import HTMLParser
from http.cookiejar import CookieJar
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode, urljoin, urlsplit
from urllib.request import (
    HTTPSHandler, HTTPCookieProcessor, HTTPRedirectHandler, ProxyHandler, Request, build_opener,
)


HERE = Path(__file__).resolve().parent
BACKEND = HERE.parents[1] / "backend-lab" / "run-smoke.py"
SCOPE = "roomcircuit-game"


def loopback_origin(base, scheme):
    parsed = urlsplit(base)
    if (parsed.scheme != scheme or parsed.hostname != "127.0.0.1" or not parsed.port or parsed.username
            or parsed.path not in {"", "/"} or parsed.query or parsed.fragment):
        raise ValueError("Private validation requires an explicit loopback origin")
    return f"{scheme}://127.0.0.1:{parsed.port}/"


class LocalRedirects(HTTPRedirectHandler):
    def __init__(self, origin):
        self.origin = urlsplit(origin).netloc

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urlsplit(newurl)
        if target.scheme != "https" or target.netloc != self.origin:
            raise RuntimeError("Browser grant attempted to leave its own HTTPS origin")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class GrantForm(HTMLParser):
    def __init__(self):
        super().__init__()
        self.csrf = None

    def handle_starttag(self, tag, attributes):
        fields = dict(attributes)
        if tag == "input" and fields.get("name") == "csrf_token":
            self.csrf = fields.get("value")


def exchange(opener, url, method="GET", data=None, headers=None):
    request = Request(url, data=data, headers=headers or {}, method=method)
    try:
        response = opener.open(request, timeout=5)
    except HTTPError as error:
        response = error
    with response:
        payload = response.read(65537)
        if len(payload) > 65536:
            raise RuntimeError("Validation response exceeded the expected size")
        return response.code, payload


def post_json(opener, origin, route, value, headers=None):
    status, body = exchange(opener, urljoin(origin, route), "POST", json.dumps(value).encode(),
                            {"Content-Type": "application/json", **(headers or {})})
    try:
        value = json.loads(body)
    except (ValueError, UnicodeError) as error:
        raise RuntimeError(f"JSON endpoint returned an invalid response (HTTP {status})") from error
    if not isinstance(value, dict):
        raise RuntimeError("JSON endpoint must return an object")
    return status, value


def grant(opener, origin, account, decision):
    status, device = post_json(opener, origin, "/v1/device/authorize", {
        "device_key": secrets.token_hex(32), "scope": SCOPE,
    })
    if status != 201 or not all(device.get(field) for field in (
            "device_code", "user_code", "verification_uri_complete", "expires_in", "interval")):
        raise RuntimeError("Device authorization did not produce a usable challenge")

    poll_data = {"device_code": device["device_code"]}
    status, pending = post_json(opener, origin, "/v1/device/token", poll_data)
    if (status != 400 or pending.get("error") != "authorization_pending"
            or pending.get("access_token") or pending.get("id_token")):
        raise RuntimeError("An unapproved device must not receive a game credential")

    browser_url = urljoin(origin, device["verification_uri_complete"])
    if urlsplit(browser_url)[:2] != urlsplit(origin)[:2]:
        raise RuntimeError("Verification URI does not belong to the independent issuer")
    status, body = exchange(opener, browser_url)
    form = GrantForm()
    form.feed(body.decode())
    if status != 200 or not form.csrf:
        raise RuntimeError("Browser approval form is unavailable")
    form_fields = {"user_code": device["user_code"], "csrf_token": form.csrf,
                   "email": account["email"], "password": account["password"], "decision": decision}
    status, _ = exchange(opener, urljoin(origin, "/device"), "POST", urlencode(form_fields).encode(),
                         {"Content-Type": "application/x-www-form-urlencoded"})
    if status != 200:
        raise RuntimeError(f"Browser grant could not be submitted (HTTP {status})")

    interval = float(device["interval"])
    deadline = time.monotonic() + min(30, float(device["expires_in"]))
    while interval > 0 and time.monotonic() + interval < deadline:
        time.sleep(interval)
        status, ticket = post_json(opener, origin, "/v1/device/token", poll_data)
        if status == 400 and ticket.get("error") == "slow_down":
            interval += 5
            continue
        if (status == 400 and ticket.get("error") == "authorization_pending"
                and not ticket.get("access_token") and not ticket.get("id_token")):
            continue
        if decision == "deny":
            if (status != 400 or ticket.get("error") != "access_denied"
                    or ticket.get("access_token") or ticket.get("id_token")):
                raise RuntimeError("A denied device must not receive a game credential")
            return None
        if status != 200 or ticket.get("scope") != SCOPE or ticket.get("token_type") != "Bearer":
            raise RuntimeError("An approved device did not receive a scoped game credential")
        if (not ticket.get("access_token") or not ticket.get("id_token")
                or ticket["access_token"] == ticket["id_token"]
                or not isinstance(ticket.get("pid"), int) or ticket["pid"] <= 0):
            raise RuntimeError("Game credential or account principal is missing")
        # Replay after the server's poll interval so a rate limit cannot mask reuse.
        time.sleep(interval)
        status, used = post_json(opener, origin, "/v1/device/token", poll_data)
        if (status != 400 or used.get("error") in {"slow_down", "authorization_pending"}
                or used.get("access_token") or used.get("id_token")):
            raise RuntimeError("A consumed device challenge issued another game credential")
        return ticket
    raise RuntimeError("Device grant polling expired")


def private_fixture(path):
    with path.open() as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ValueError("Validation fixture must be an owner-only JSON file")
        return json.load(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--source", type=Path)
    inputs.add_argument("--binary", type=Path)
    args = parser.parse_args()
    config = private_fixture(args.fixture)
    origin = loopback_origin(config["issuer"], "https")
    identity = loopback_origin(config["RC_IDENTITY_URL"], "http")
    ca = (args.fixture.parent / config["ca_file"]).resolve()
    tls = ssl.create_default_context(cafile=str(ca))
    tls.minimum_version = ssl.TLSVersion.TLSv1_2
    accounts = [config["host"], config["guest"]]
    if accounts[0]["email"] == accounts[1]["email"]:
        raise ValueError("Two distinct independently verified test accounts are needed")

    def browser():
        return build_opener(ProxyHandler({}), HTTPSHandler(context=tls), HTTPCookieProcessor(CookieJar()),
                            LocalRedirects(origin))

    grant(browser(), origin, accounts[0], "deny")
    print("PASS browser-denied device was refused a game credential", flush=True)
    host, guest = [grant(browser(), origin, account, "approve") for account in accounts]
    if host["pid"] == guest["pid"]:
        raise RuntimeError("Independent accounts resolved to the same game principal")

    callback = build_opener(ProxyHandler({}), LocalRedirects(origin))
    private = {"X-Internal-Key": config["RC_IDENTITY_KEY"]}
    for ticket in (host, guest):
        status, result = post_json(callback, identity, "/internal/switch/identity",
                                   {"id_token": ticket["id_token"]}, private)
        if status != 200 or result.get("pid") != ticket["pid"] or not result.get("account_id"):
            raise RuntimeError("Game credential resolved to the wrong independent account")
        status, _ = post_json(callback, identity, "/internal/switch/identity",
                              {"id_token": ticket["access_token"]}, private)
        if status not in {401, 403}:
            raise RuntimeError("App access token was accepted as a game identity")
    if config.get("legacy_token"):
        status, _ = post_json(callback, identity, "/internal/switch/identity",
                              {"id_token": config["legacy_token"]}, private)
        if status not in {401, 403}:
            raise RuntimeError("Credential from another issuer was not rejected")
        print("PASS foreign issuer credential was refused", flush=True)
    print("PASS two browser-approved single-use device grants resolved to distinct accounts", flush=True)
    print("PASS app access tokens were rejected at the native game identity boundary", flush=True)

    with tempfile.NamedTemporaryFile(mode="w", prefix=".native-validation-", dir=HERE) as credentials:
        json.dump({"RC_IDENTITY_URL": identity.rstrip("/"), "RC_IDENTITY_KEY": config["RC_IDENTITY_KEY"],
                   "RC_HOST_TOKEN": host["id_token"], "RC_GUEST_TOKEN": guest["id_token"],
                   "RC_HOST_PID": host["pid"], "RC_GUEST_PID": guest["pid"]}, credentials)
        credentials.flush()
        backend_input = ["--source", str(args.source)] if args.source else ["--binary", str(args.binary)]
        subprocess.run(["python3", "-B", str(BACKEND), *backend_input, "--credentials", credentials.name], check=True)


if __name__ == "__main__":
    main()
