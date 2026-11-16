# Independent native account and game protocol validation

The source-level path is an independently verified account, a browser-approved device grant, a scoped credential, title-server identity verification, a fresh game login ticket and an authenticated room. The game fixture performs real loopback TLS and NEX connections and exercises the PICO PARK and Mario Party Superstars profiles. It covers the game service's account and lobby protocol. A retail game's console trust path and actual gameplay require separate device evidence.

## Account to game contract

| Boundary | Data and check |
| --- | --- |
| Device authorization | A client asks the independent issuer for `roomcircuit-game` scope. A verified account approves the pending device request in the independent browser form. Pending or denied requests must not return a game token. |
| Game identity verification | `POST /internal/switch/identity` receives the issued `id_token` over the private interface with `X-Internal-Key`. It rejects the separate app access token. A valid game grant resolves to an account with stable `pid`, its `baas_user_id` principal, `account_id`, nickname, country and friends. This is the service-to-service shape used by the upstream OpenPak game server. |
| NEX ticket and room | The game realm checks that identity before signing a game ticket, refuses an unverified login, and requires a ticket on the secure stage. Its receiver confirms that the two authenticated account principals take part in the exact same room. |

OpenPak's identity callback proves the token before using any application claim. Its separate `nsoMembership` handler reports active membership independently of the requester's signed-in state. The device-grant service issues only the independent `roomcircuit-game` scope. The NEX fixture uses its own game transport and ticket keys and verifies TLS against its explicitly trusted certificate. [OpenPak identity callback](https://github.com/openpak/nx-baas/blob/cf93fd71d05296ac321a90fa880af707bf35ed44/internal/gameapi/internal.go#L31-L115) · [Membership handler](https://github.com/openpak/nx-baas/blob/cf93fd71d05296ac321a90fa880af707bf35ed44/internal/services/nso.go#L158-L172) · [NEX fixture](../../backend-lab/authenticated_room_test.go)

## HOS 19.0.1 and Atmosphère 1.12.0 E

- RoomCircuit already registers `ldn:u` and `bsd:u` with Atmosphère, plus its own `lanywhr` application service. A second MITM for the same service returns `AlreadyRegistered`. The own backend requires no change to those services. [RoomCircuit service registration](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/console/ldn_mitm/source/ldnmitm_main.cpp#L257-L263) · [Atmosphère 1.12.0 service manager](https://github.com/Atmosphere-NX/Atmosphere/blob/1.12.0/stratosphere/sm/source/impl/sm_service_manager.cpp#L726-L751)
- An emuMMC boot identifies the system image. The SD title directory and startup markers may still be shared by other boot entries, so a boot-launched sibling module would require a separate isolation and cold-boot test. The backend fixture does not add a startup module, certificates or system account state to the console.
- The NEX issuer lookup is bounded by the upstream five-second HTTP timeout. The validation script has bounded JSON/HTML reads and a finite device challenge deadline. The RoomCircuit module separately configures 48 sessions and 12 server threads with `0x4000`-byte stacks. Adding blocking socket calls or another startup path would require testing that it cannot hold those workers or exhaust the module's memory. [RoomCircuit service budgets](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/console/ldn_mitm/source/ldnmitm_main.cpp#L93-L149)
- OpenPak's current public Switch client has a narrower release matrix than Atmosphère itself. Its independent-network documentation also distinguishes deployable services from console-client trust. Applicability to this console needs a supported client and a real test with the exact HOS and Atmosphère pairing. [Switch client support](https://docs.openpak.org/players/setup/switch/) · [Independent network](https://docs.openpak.org/self-hosting/)

## Reproduce the independent backend tests

Use the account lab's running own HTTPS grant service and protected identity endpoint. Provide its owner-only browser fixture, which contains `issuer`, `ca_file`, `RC_IDENTITY_URL`, `RC_IDENTITY_KEY`, and the synthetic `host` and `guest` accounts. Each test account supplies `email` and `password`. The CA path can be relative to that fixture. An optional `legacy_token` allows a rejection check for a credential from the earlier separate issuer. The fixture stays outside version control.

```sh
python3 -B native-validation/backend/run-device-game-validation.py \
  --fixture /path/to/private-own-realm-fixture.json \
  --source /path/to/openpak-switch-nex-v0.3.45
```

The same runner takes `--binary /path/to/roomcircuit-nex-smoke-linux-amd64` with the [prebuilt Linux test executable](../../backend-lab/README.md). It checks denied grants and consumed challenges before using two fresh device-issued credentials in the game fixture.
