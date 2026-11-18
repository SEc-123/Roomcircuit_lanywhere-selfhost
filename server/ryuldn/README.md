# RyuLDN on RoomCircuit

The optional RyuLDN v1 listener uses the same RoomCircuit rooms as the Switch client. It proxies game UDP and TCP and uses the RoomCircuit server for all transport. Crossplay is available in open local wireless rooms. Use the same game update on every device.

## Ryubing and Kenji-NX desktop

Enable RyuLDN in Network settings, leave the network passphrase blank, and disable P2P network hosting and guest Internet access. Then close the emulator and set its top-level `ldn_server` configuration field to the IPv4 address or IPv4-only DNS name of the RoomCircuit host. The emulator uses TCP port 30456 automatically. Enter the host without a port.

This works through the configuration reader in Ryubing source `6af327bdd50366163d80bc692f6bf47ec1c1993e` and Kenji-NX desktop source `506e2c7a69b4f801a519857e465e81834d618da8`. Their desktop settings pages expose the mode and P2P toggle. Edit the host in the configuration file.

## Kenji-NX Android development build

The reviewed Android source `280c427c550c5fe3bcc46b378e06a9b90a7e6b09b` initializes multiplayer in Disabled mode. Apply `kenji-android-ldn-server.patch` to that source and build with the upstream arm64 Android toolchain. The patch adds **System → Local wireless server**. Save the IPv4 address or IPv4-only DNS name, then relaunch the game and select local wireless. Clear and save the field to turn off that mode.

The patch applies to the pinned upstream sources. An APK build and phone runtime are still to be validated.

## Reference wire check

Run `python3 server/ryuldn/probe-upstream.py /path/to/LdnServer` with a clean checkout of [Ryubing/LdnServer](https://github.com/Ryubing/LdnServer) at `ba32bcf0878184b3f30b3db4e2d3888a08aa17dc`. The probe uses upstream wire codecs against a local RoomCircuit server and checks create, scan, join, synchronization, game UDP and TCP in both directions, stream close, and host departure.
