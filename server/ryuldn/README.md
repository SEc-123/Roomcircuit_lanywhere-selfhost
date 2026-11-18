# RyuLDN on RoomCircuit

The optional RyuLDN v1 listener uses the same RoomCircuit rooms as the Switch client. It proxies game UDP and TCP and uses the RoomCircuit server for all transport. Crossplay is available in open local wireless rooms. Use the same game update on every device.

## Ryubing and Kenji-NX desktop

Enable RyuLDN in Network settings, leave the network passphrase blank, and disable P2P network hosting and guest Internet access. Then close the emulator and set its top-level `ldn_server` configuration field to the IPv4 address or IPv4-only DNS name of the RoomCircuit host. The emulator uses TCP port 30456 automatically. Enter the host without a port.

This works through the configuration reader in Ryubing source `6af327bdd50366163d80bc692f6bf47ec1c1993e` and Kenji-NX desktop source `506e2c7a69b4f801a519857e465e81834d618da8`. Their desktop settings pages expose the mode and P2P toggle. Edit the host in the configuration file.

For games that block while receiving UDP, use connected UDP sockets, or use game TCP, apply `ryu-client-sockets.patch` to the matching emulator checkout before building. The stock client revisions can stall during these socket operations. The patch wakes blocked game reads, completes TCP connect and accept, preserves the peer endpoint and drains queued bytes on orderly close. It also keeps accepted connections alive when their listener closes.

## Kenji-NX Android development build

The reviewed Android source `280c427c550c5fe3bcc46b378e06a9b90a7e6b09b` initializes multiplayer in Disabled mode. Apply both `kenji-android-ldn-server.patch` and `ryu-client-sockets.patch` to that source and build with the upstream arm64 Android toolchain. The settings patch adds **System → Local wireless server**. Save the IPv4 address or IPv4-only DNS name, then relaunch the game and select local wireless. Clear and save the field to turn off that mode.

The patch applies to the pinned upstream sources. An APK build and phone runtime are still to be validated.

## Reference wire check

Run `python3 server/ryuldn/probe-upstream.py /path/to/LdnServer` with a clean checkout of [Ryubing/LdnServer](https://github.com/Ryubing/LdnServer) at `ba32bcf0878184b3f30b3db4e2d3888a08aa17dc`. The probe uses upstream wire codecs against a local RoomCircuit server and checks create, scan, join, synchronization, game UDP and TCP in both directions, stream close, and host departure.

Run `python3 server/ryuldn/client-test/run.py /path/to/emulator CHECKOUT_COMMIT --docker` for any one of the three pinned emulator revisions above. It compiles the patched upstream C# socket class and wire types, tests blocking and close behavior, then connects two instances through a transport fixture to an actual RoomCircuit relay. Docker uses the .NET 9 SDK and Node 24; with both installed locally, omit `--docker`. Device OS networking and commercial game behavior require separate hardware validation.
