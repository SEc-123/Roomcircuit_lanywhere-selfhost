# Native RoomCircuit for MeloNX and NX Pocket

The patches add SwiftUI appointment screens. They use the native API of the same RoomCircuit relay as the Switch app. Each client can list appointments, create one, get ready or cancel ready, and find compatible open rooms. Followed appointments are saved per relay and checked every 20 seconds while the panel is visible and the app is active. The check cycles through up to three followed appointments each time. A newly open room shows an in-app notice. There is no background or system push notification.

| Source patch | Verified upstream revision | Native entry |
| --- | --- | --- |
| [melonx-roomcircuit.patch](melonx-roomcircuit.patch) | [Official MeloNX](https://starforgejo.dev/projects/MeloNX) at `13700519f3abc4e6d7ed8a065b118b2cc59bcd67` | Settings → Misc → Network → RoomCircuit |
| [nx-pocket-roomcircuit.patch](nx-pocket-roomcircuit.patch) | [NX Pocket](https://github.com/zkkzkk/nx-pocket) at `9c80cb793df2c2032e3f01153585a13150c7925d` | iPhone/iPad: Settings → RoomCircuit, or from Local play. Mac: View → RoomCircuit |

## Apply and use

1. In a clean upstream checkout at the listed revision, run `git apply --check /path/to/patch`, then `git apply /path/to/patch`.
2. Build the app with its upstream pipeline. MeloNX uses `distribution/ios/build.sh` for its emulator library and the MeloNX Xcode project for the app. NX Pocket uses `dm/ios/tools/build_pocket.py` for the iPhone app and `dm/mac/pocket/build.sh` for its Mac app. Keep the normal device signing setup.
3. Open the native RoomCircuit entry, enter the operator's appointment server origin, such as `https://rooms.example.net`, and connect. The relay must advertise `nativeApi: 1`. Each relay receives a different random ID saved within the app. An address is saved only after a successful options and list request.
4. When a compatible room appears, open the game and join from its Local Wireless menu. The native screen displays a LAN Play address only if that relay has LAN Play enabled.

For the local preview, the appointment origin is `http://<relay-LAN-IP>:11464`. The apps request local network permission for a LAN address. Public hosts require HTTPS with normal platform certificate validation. Open-room notices work while the panel is visible. Ready is governed by the same server lease as on Switch. [Apple local network access](https://developer.apple.com/documentation/bundleresources/information-property-list/nslocalnetworkusagedescription) · [Apple local ATS settings](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)

The appointment code does not change emulation or add a LAN Play tunnel to MeloNX or NX Pocket. Play still needs the appropriate Apple Wi-Fi gateway and a compatible host build. In particular, this is a patch for the official current MeloNX tree, not a rebase of the older separately tested MeloNX gateway patch. See [crossplay setup](../../docs/CROSSPLAY.md) for the game path and [native integrations](../../docs/NATIVE-INTEGRATIONS.md) for shared service setup.

## Reproduce

From the RoomCircuit checkout:

```sh
python3 integrations/apple/make_patches.py --check --melonx /path/to/melonx --nx-pocket /path/to/nx-pocket
python3 integrations/apple/tests/verify.py
```

The first command regenerates from the pinned Git objects, checks forward and reverse application on isolated copies, verifies the Swift attachment syntax, and verifies MeloNX's local networking property list. The second type-checks the SwiftUI panel for macOS and arm64 iOS and runs the compiled Swift client against an isolated, real Node relay. The probe checks server options, creation, shared visibility with different IDs, readiness, detail, rooms, and leaving ready.

These are source patches. The Swift modules and shared endpoint flow were verified, but a signed iOS app and an on-device game session were not built or tested. The patches are not upstream merges or released emulator updates.
