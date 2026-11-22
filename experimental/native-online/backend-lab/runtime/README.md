# RoomCircuit independent NEX runtime

This builds an isolated executable from [OpenPak Switch NEX v0.3.45](https://github.com/openpak/switch-nex/tree/v0.3.45). The overlay gates startup on a loopback identity service, its private key, a separate private ticket secret, an explicit advertised station and one or both selected title profiles. Both game listeners use TLS 1.2 or later. The configuration refuses an unverified name login and binds the game listeners to loopback by default. Each title uses its own identity state and validates the authorized application ID on login, reconnect and periodic admission checks. The health listener remains loopback-only. A realm's ticket secret derives a separate ticket key per title. [Runtime policy](policy.go) · [Source overlay](build.py)

Every secure connection verifies the ticket's issue time and checks its principal against the live independent admission before acknowledging CONNECT, including after a process restart. New connections require tickets at most two hours old, with up to one minute of forward clock skew. An established connection checks its active admission every 30 seconds and closes on confirmed revocation or a missing lease. Temporary identity-service failures preserve established connections. Short login credential expiration alone does not end an active game.

| Title | Application ID | Reference game server ID | Auth TCP | Secure TCP | Local health |
| --- | --- | --- | --- | --- | --- |
| Mario Party Superstars | `01006FE013472000` | `28abaa00` | `22110` | `22111` | `22112` |
| PICO PARK | `01004EA00DF70000` | `2c3c9a00` | `23080` | `23081` | `23082` |

The pinned registry supplies the public game transport constants, `prudps` station scheme and title-specific RPC behavior. The referenced application ID, application version, and game server ID serve different purposes. The application's actual build needs to reach the matching independent auth listener through an authorized client route. Its secure ticket advertises an IP and secure port, so its TLS certificate needs that IP in the SAN. [Runtime registry](https://github.com/openpak/switch-nex/blob/v0.3.45/cmd/switch-nexd/main.go) · [Mario Party source provenance](https://github.com/openpak/nex/blob/main/titles/mario-party-superstars/docs/provenance.md) · [PICO PARK profile](https://github.com/openpak/switch-nex/blob/v0.3.45/docs/nex-first-batch.md) · [Station format](https://github.com/openpak/switch-nex/blob/v0.3.45/SPEC.md)

Build with Go 1.26 after populating the pinned checkout's dependency cache. Add `--linux` for a static Linux x86-64 executable. `--test` verifies configuration and pre-bind startup refusal. Add `--credentials` with an owner-only fixture from the independent account adapter to test the matching real profile listener and the cross-title denial with two verified test accounts. Set `NX_LAB_APPLICATION` to the actual title ID before generating the fixture; the synthetic default is not accepted by this runtime test. The test uses the reference profile's public PRUDP settings and a fresh locally trusted certificate and private ticket secret.

```sh
python3 -B build.py --source /path/to/switch-nex-v0.3.45 \
  --binary /path/to/roomcircuit-nexd --test \
  --credentials /path/to/private-game-fixture.json
```

Supply `NX_INTERNAL_URL`, `NX_INTERNAL_KEY` and `RC_NEX_TICKET_KEY` through the deployment's private environment. Both keys require 32 random bytes in hex. Keep `NX_INTERNAL_URL` on the same host, such as `http://127.0.0.1:39283`. Supply the key and certificate file paths on launch. This explicit loopback example uses both port blocks.

```sh
/path/to/roomcircuit-nexd -only=mario-party-superstars,pico-park \
  -station-address=127.0.0.1 -cert=/private/realm.pem -key=/private/realm.key
```

An owned service deployment may bind a chosen interface with `-bind-address` and advertise its reachable IPv4 endpoint with `-station-address`. `-auth-proxy-peer` enables PROXY protocol on the authentication listener from exactly one configured private ingress peer. The secure listener receives TLS directly. Keep the callback and health services private. Verify the configured identity callback and both bound ports before exposing the game listeners.

The Linux verification package adds the static matching test client and `run-linux.sh`. With the account adapter already running, run `./run-linux.sh /path/to/local/game-test.json` as the fixture owner while both title port blocks are free. The binaries generate a fresh loopback certificate and private ticket secret, exercise the live callback, and stop the test process when done. A local test authority exercises game-server restart, login credential expiry, unknown and unavailable reconnects, and a live connection revoked during the 30-second sweep. A clock-controlled unit test checks the precise ticket-age and skew limits. The fixture stays outside the package. The command uses the shell and the two binaries, with no installed Go toolchain.

The adapter implements proof of an independent account and its approved grant. It needs a separate observed client flow to issue the right title identity from a HOS local profile. The two-account CLI fixture specifies the actual application ID. The matching title exercises a room lifecycle, and the other title must reject the same credentials. Until separate title-aware routing is deployed, configure one matching title per adapter and NEX process. It does not prove the retail game's title admission, register its real peer station, or establish its NAT traversal. OpenPak's measured SDK NAT path needs separate responder addresses; verify the selected game's requirement and actual traffic before claiming playable online support. [Native client review](../../native-validation/backend/native-client-gap-review.md) · [NAT reference](https://github.com/openpak/switch-nex/blob/v0.3.45/SPEC.md#pia--nat-check)
