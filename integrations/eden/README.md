# Eden native RoomCircuit

The patches target [Eden](https://git.eden-emu.dev/eden-emu/eden) at commit `d85bad89248fcae68f812d7e1c937436261404cb`.

| Build | Entry | Source patch |
| --- | --- | --- |
| Desktop Qt | Multiplayer → RoomCircuit appointments | [eden-desktop.patch](eden-desktop.patch) |
| Android | Eden home → Settings → RoomCircuit | [eden-android.patch](eden-android.patch) |

The dialogs load the same server appointments and ready counts as the Switch. They can create, refresh, page, follow, mark or cancel ready, check compatible open rooms and show a local alert for a followed appointment with an open room. Follows and the random device identity are saved locally per service. Room polling runs while the dialog is open.

**Direct connect** opens Eden's existing networking dialog with the relay's Eden address and port. The player confirms the connection, starts the matching title and uses the game's Local Wireless menu. The panels do not launch a browser. Use the same game version across devices.

## Build and configure

Start with a clean Eden checkout. Apply the patch for the intended build. Both patches can coexist. Leave Eden's own Gradle and CMake build process in place.

```sh
git switch --detach d85bad89248fcae68f812d7e1c937436261404cb
git switch -c roomcircuit-native
git apply --check /path/to/RoomCircuit/integrations/eden/eden-desktop.patch
git apply /path/to/RoomCircuit/integrations/eden/eden-desktop.patch
# Apply eden-android.patch instead, or as well, for an Android build.
```

Build with the upstream [build guide](https://git.eden-emu.dev/eden-emu/eden/src/branch/master/docs/Build.md). The desktop target also links Qt Network. On Android, this patch follows the app's cleartext policy and requires an HTTPS RoomCircuit site. The desktop build additionally accepts explicit private-network or loopback HTTP for a local preview.

In RoomCircuit, enable the appointments web service and an Eden game listener. Use the service origin, without a path or credentials, in the Eden panel. The service must expose `nativeApi: 1` at `/api/appointments/options` and accept `POST /api/appointments/native`. [Server setup](../../docs/WEB-APPOINTMENTS.md)

## Verify

Both patches pass `git apply --check` against clean source files fetched at the upstream pin above. The Android dialog compiled for Java 17 against the local Android API 37 SDK with generated resource IDs. AAPT2 from build-tools 36 compiled the new English and Chinese resources. The extracted Kotlin prefill callback compiled against the panel and upstream signatures. The desktop dialog and smoke harness pass C++20 syntax and type checks against Qt 6.4 Widgets and Network headers. These are component checks, not complete desktop or Android builds or a device session.

For a targeted Desktop dialog smoke test on a Linux host with Qt 6 development packages, set the commands below from the patched Eden checkout. Android and desktop emulator packages can be built with their upstream project commands. The test creates an appointment, so use a dedicated relay instance with empty state and an Eden game listener.

```sh
integration=/absolute/path/to/RoomCircuit/integrations/eden
c++ -std=c++20 -fPIC -Isrc/yuzu/multiplayer \
  "$integration/verify-dialog.cpp" src/yuzu/multiplayer/roomcircuit_dialog.cpp \
  $(pkg-config --cflags --libs Qt6Widgets Qt6Network) -o /tmp/verify-eden-dialog
config_dir=$(mktemp -d)
QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$config_dir" \
  /tmp/verify-eden-dialog http://127.0.0.1:11464 127.0.0.1 24872
```

The deliverables are source patches. Full desktop packages, Android APKs, physical gameplay, and upstream acceptance are pending. Eden's [contribution rules](https://git.eden-emu.dev/eden-emu/eden/src/branch/master/CONTRIBUTING.md) require authors to write their own PR text and disclose AI-assisted code. Its repository instructions also exclude agent submission or pushes on a user's behalf.
