# MeloNX on a RoomCircuit Wi-Fi gateway

The patch targets MeloNX commit `51763377a60a662f9eed2df57745aa7deebdf8b9`. Stock builds at that revision discover rooms by sending UDP broadcasts. iOS requires a multicast entitlement for that operation. The patch lets a player select the RoomCircuit gateway and uses ordinary IPv4 unicast on the phone. [Apple local network guidance](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)

1. Apply `melonx-wifi-gateway.patch` to that revision and rebuild both the native emulator core and the iOS app with the MeloNX build tools. Install your signed build.
2. Connect the phone to the [RoomCircuit test Wi-Fi](../../docs/CROSSPLAY.md). Select its `10.13.x.x` Wi-Fi interface in MeloNX. Turn on **ldn_mitm** and **RoomCircuit Wi-Fi gateway**, then start the game. Allow local network access.
3. Create or search for a room in the game's Local Wireless mode.

The phone uses UDP 11452 on the reserved virtual address `10.13.254.254` for discovery. It continues to connect directly to the advertised host virtual IP on TCP 11452. During a game session the optional route sends subnet and limited UDP broadcasts to the same gateway address, preserving game ports. The relay distributes those datagrams to admitted players in the same game room. Unicast game destinations retain their peer address.

Run `python3 server/ryuldn/melonx-verify.py /path/to/melonx --docker` from the RoomCircuit checkout to check patch application, parse the Swift sources when Swift is available, and run the portable C# route tests under .NET 9. The Docker run compiles the patched socket methods and uses real UDP sockets in an isolated namespace to verify unconnected send and connected send with broadcast permission disabled. The deliverable is a source patch. A full native iOS build, signing and gameplay on a physical iPhone require separate verification.

The same patch also waits for the TCP completion callback before sending a join and checks the actual SSID bytes during filtered scans.

For a provider-level test, build `roomcircuit-melonx-reference:local` from `server/ryuldn/melonx-reference/Dockerfile` and run `python3 server/ryuldn/melonx-reference/prepare.py /path/to/melonx /tmp/melo-provider`. With the pinned NX Pocket checkout and its reference image prepared, run `NX_POCKET_SOURCE=/path/to/nx-pocket MELONX_REFERENCE_BUILD=/tmp/melo-provider node server/ryuldn/melonx-reference/test.js`. The suite runs the patched MeloNX discovery and socket sources with NetCoreServer 8.0.7 in Linux using a real TUN interface. Each role scans or hosts, waits for both members and exchanges game UDP. It covers Switch, RyuLDN, Eden, legacy Yuzu, NX Pocket and a second MeloNX provider. The harness supplies the game service callback, username, and game data. iOS network permissions, the full emulator and physical gameplay need separate validation.
