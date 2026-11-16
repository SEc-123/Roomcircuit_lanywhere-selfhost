# Independent game backend smoke test

The test runs the OpenPak Switch NEX transport and matchmaking handlers in a private `roomcircuit-lab` realm. It uses a generated TLS certificate trusted by the test client, a generated transport key and a generated server ticket key. The transport binds to loopback. It exercises public matchmaking profiles for PICO PARK and Mario Party Superstars. The Mario Party case also verifies its six bootstrap settings.

The game backend validates session credentials through the independent account service. That service hosts `POST /internal/switch/identity`, accepts an opaque account credential in `id_token` and requires the internal service credential in `X-Internal-Key`. It returns distinct stable game principals for two different accounts. Provision the accounts through the account lab and provide the owner-only JSON file that contains `RC_IDENTITY_URL`, `RC_IDENTITY_KEY`, `RC_HOST_TOKEN`, `RC_GUEST_TOKEN`, `RC_HOST_PID` and `RC_GUEST_PID`.

Use Go 1.26 and a local checkout of [OpenPak Switch NEX v0.3.45](https://github.com/openpak/switch-nex/releases/tag/v0.3.45) at `2af4741d321f9d9f9aa45f96454137f86130c288`. Download the public Go dependencies once. The smoke runner then compiles with `GOPROXY=off` and uses a Go overlay to keep the upstream source untouched.

```sh
git clone --depth 1 --branch v0.3.45 https://github.com/openpak/switch-nex.git /path/to/switch-nex
git -C /path/to/switch-nex rev-parse HEAD
(cd /path/to/switch-nex && GOWORK=off GOTOOLCHAIN=local go mod download)
```

```sh
python3 -B run-smoke.py --source /path/to/switch-nex --credentials /path/to/private-account-smoke.json
```

To run on Linux x86-64 without installing Go, build the test executable first.

```sh
python3 -B run-smoke.py --source /path/to/switch-nex --linux-binary roomcircuit-nex-smoke-linux-amd64
python3 -B run-smoke.py --binary ./roomcircuit-nex-smoke-linux-amd64 --credentials /path/to/private-account-smoke.json
```

The test checks rejected credentials, account-bound login tickets, distinct secure-stage identities and room creation, discovery, joining and cleanup across real loopback connections. The private account and game protocol validation does not establish retail Switch client or actual gameplay support.
