# LAN Play IPv4 transport

This directory vendors the BSD-licensed lwIP 2.2.1 core pinned in `UPSTREAM.json`.
The upstream `src/core`, `include/lwip` and `include/netif` files are unmodified.
`include/arch/cc.h`, `include/lwipopts.h` and `Makefile` are the local port.

`../ldn_mitm/source/lanplay_direct.cpp` runs the IPv4 stack on one serialized worker.
The Switch sends raw IPv4 over the selected public node's UDP protocol; no PC,
TUN device or central game-traffic relay is involved. Standard LDN control and
game TCP use lwIP retransmission, reassembly, windows and stream framing. Legacy
fork TCP follows that peer's existing UDP stream protocol and cannot add
acknowledgments that the legacy peer does not support.

Memory and queue limits are fixed. Host and Switch objects use different build
directories, and header dependencies invalidate both builds correctly. The
virtual address is retained on the SD card; a detected collision ends the old
session and renews the address before reopening. UDP socket rebinding preserves
the selected node, address and session epoch.

Run the portable production-code tests from the repository root:

```sh
python3 console/ldn_mitm/tests/lanplay_direct_test.py
```

The fixtures verify raw standard/fork discovery, exact selection, both hosting
directions, bidirectional game UDP/TCP, large UDP fragmentation, TCP packet-loss
recovery, crowded scans, address collisions and socket rebinding. These tests
exercise real sockets and wire peers, not a mocked success path. They do not
replace two physical Switches playing the same game version.
