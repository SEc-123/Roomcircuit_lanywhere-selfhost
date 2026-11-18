# Upstream RoomMember network checks

Run these probes from a checkout with Node.js 24, a C compiler, a C++20 compiler and curl.

```sh
server/eden/reference/verify.sh
server/eden/reference/verify.sh --yuzu
```

The script fetches source files pinned by commit and SHA-256 from Eden's forge, including the preserved original Yuzu history. It compiles the upstream RoomMember and Packet implementations unchanged with ENet 1.3.18. A local assertion shim removes the dependency on the emulator's logging frontend and still checks the assertions. Set `EDEN_SOURCE` to a matching existing upstream checkout to use local files, and optionally set `ROOMCIRCUIT_ENET_ARCHIVE` to a local ENet 1.3.18 source archive.

Each run starts an isolated RoomCircuit relay on loopback. The Switch protocol fixture and compiled emulator client take turns hosting. The actual RoomMember reads the lobby roster and carries the LDN game discovery, join, update and leave messages. Both roles exchange game payloads, with an exact byte comparison in each direction. The harness supplies game/HLE inputs and uses Node's Zstandard codec to generate and inspect payloads. The emulator's graphical frontend, its guest socket/HLE implementation, Switch hardware and commercial games are outside this check.

Eden `d85bad89248fcae68f812d7e1c937436261404cb` uses UDP enum 4. Yuzu `74d0865478679c38a3bd24b5f02a3b2d3be662f7` uses UDP enum 3. Both report room protocol version 1, so the test starts the listener with the appropriate profile. The same IPv4 address can serve both profiles on their separate UDP ports. Unsupported TCP messages are ignored on each correct listener. The handshake has no field to infer a client selecting the wrong port.
