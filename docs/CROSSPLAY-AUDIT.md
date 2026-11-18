# Crossplay verification

The preview relay was exercised with five wire profiles. Each off-diagonal cell below includes discovery, admission and UDP game bytes in both directions with the indicated host. The Switch column uses RoomCircuit's native wire protocol. The two routed columns run the pinned local-wireless implementations from NX Pocket and patched MeloNX through kernel sockets and the shipped Linux TUN gateway. Both use the standard LAN Play transport.

| Host / guest | Native Switch wire | Eden (UDP=4) | Yuzu (UDP=3) | RyuLDN | NX Pocket LDN | MeloNX LDN |
| --- | --- | --- | --- | --- | --- | --- |
| Native Switch wire | Pass | Pass | Pass | Pass | Pass | Pass |
| Eden (UDP=4) | Pass | Pass | Pass | Pass | Pass | Pass |
| Yuzu (UDP=3) | Pass | Pass | Pass | Pass | Pass | Pass |
| RyuLDN | Pass | Pass | Pass | Pass | Pass | Pass |
| NX Pocket LDN | Pass | Pass | Pass | Pass | Pass | Pass |
| MeloNX LDN | Pass | Pass | Pass | Pass | Pass | Pass |

The relay separates concurrent mixed rooms for the same game. Tests also cover rehosting at the same IP, leaving and joining a new room, room discovery with over 100 entries, and bidirectional incompressible UDP payloads up to 65,507 bytes between the Eden/Yuzu profiles and RyuLDN. The standard LAN Play transport retains its shared-LAN behavior for legacy peers. The routed iOS game broadcast uses admitted room membership.

## What the clients require

- **Switch.** The configurable SD test build can select this relay. Native wire tests run normal TCP and UDP connections against the server. Physical console timing, game menus, a completed match and a Pokémon trade need device testing.
- **Eden and Yuzu.** Eden uses reliable ENet packets for LDN state and Zstandard for game datagrams. Eden and historical Yuzu share a handshake but encode UDP as 4 and 3 respectively. Eden v0.2.1 uses UDP port 24872. Original Yuzu at `74d086547867` and Eden v0.2.0 use port 24873. The two Eden tags have identical room headers, RoomMember and socket proxy implementation files; the socket protocol enum changed in `46cfd9b6`. The [upstream RoomMember tests](../server/eden/reference/README.md) exercise the compiled network implementations independently. Game TCP connect/listen/accept are unimplemented in the reviewed socket proxy. A fork that changes its network protocol needs separate verification.
- **Ryubing and Kenji.** The relay supports UDP and proxied TCP. The pinned clients have socket lifecycle bugs affecting blocking receive, TCP connect/accept and orderly close. Two patched upstream socket classes exchange blocking UDP and TCP through a real RoomCircuit server. A test adapter supplies transport dispatch and game calls. The [client instructions](../server/ryuldn/README.md) describe the patches and the pinned desktop/Android revisions. Kenji Android additionally requires its server-selection patch. Apply both changes when building that APK.
- **NX Pocket.** The pinned source is `9c80cb793df2c2032e3f01153585a13150c7925d`. Its own macOS LDN tests pass. Ten routed runs exercise its discovery, host and station state machines against each family through real Linux TCP/UDP and TUN. Those runs use its LDN source with Darwin socket declarations adapted for Linux and host shims for interface selection and HLE events. The payload exchange uses kernel UDP sockets. This covers the wire path used by games with an enabled Local play profile.
- **MeloNX.** The reviewed iOS build relies on UDP broadcast discovery and lacks the multicast entitlement used by iOS for that operation. The [opt-in gateway patch](../server/ryuldn/melonx.md) sends discovery and in-game broadcasts to a unicast address. The compiled upstream discovery, protocol and control-socket classes were tested with NetCoreServer 8.0.7 and a game-callback fixture. The patched provider hosted and joined in each of twelve scenarios through Linux TUN, including MeloNX to NX Pocket and MeloNX to MeloNX. Tests also check matching and mismatching SSIDs. See the [provider test](../server/ryuldn/melonx-reference). Device signing and a full iPhone game session are unverified.

The tested standard LAN Play gateway uses TCP 11452 for LDN joining. In a mixed LAN Play/Switch room, this does not provide general TCP gameplay. The tests use generated traffic and source-level clients. The emulator bridge requires a concrete local communication ID. A client that sends an unresolved default ID (`0xffffffffffffffff`) cannot publish or join through that bridge until the game ID is resolved; the Switch module uses the title’s own control metadata. Password-protected admission, private matching and a successful trade in a specific Pokémon version are outside this matrix. No commercial game ran in these checks.

## Reproduce

From the source checkout, build the ENet worker and client with `ROOMCIRCUIT_ENET_BUILD_TEST=1 sh server/eden/build.sh`. Run:

```sh
node server/eden/test-host.js
node server/eden/test-crossplay.js
ROOMCIRCUIT_TEST_YUZU=1 node server/eden/test-crossplay.js
node server/eden/test-ryuldn-crossplay.js
node --test server/relay/emulator-bridge.test.js server/ryuldn/test-streams.js
node server/relay/test-lanplay.js
```

For the Apple reference test, check out the NX Pocket revision above. The harness verifies the SHA-256 of its LDN translation unit before compiling it. The temporary Linux containers need a TUN device and NET_ADMIN.

```sh
docker build -t roomcircuit-reference-sockets:local tools/lanplay-tun/reference
NX_POCKET_SOURCE=/absolute/path/to/nx-pocket node tools/lanplay-tun/reference/test-nx.js
```
