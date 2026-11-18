# Native RoomCircuit for Ryubing and Kenji-NX

These are emulator source integrations. The desktop builds add **RoomCircuit** to the Avalonia menu. The Kenji Android build adds **RoomCircuit** under **Settings → System**. The panel lists and paginates appointments, creates new events, marks or clears readiness, follows events locally, shows their open crossplay rooms, and copies share links. It uses the relay's native API and the same appointment store as the Switch and web clients.

## Supported source revisions

| Patch | Upstream | Revision |
| --- | --- | --- |
| [Ryubing desktop](patches/ryubing-desktop.patch) | [Ryubing](https://git.ryujinx.app/projects/Ryubing) | `6af327bdd50366163d80bc692f6bf47ec1c1993e` |
| [Kenji desktop](patches/kenji-desktop.patch) | [Kenji-NX](https://git.ryujinx.app/projects/Kenji-NX) | `dfd2bc69bfc5e0071132c056860d452f1a5b9b41` |
| [Kenji Android](patches/kenji-android.patch) | [Kenji-NX](https://git.ryujinx.app/projects/Kenji-NX/src/branch/libryujinx_bionic) | `280c427c550c5fe3bcc46b378e06a9b90a7e6b09b` |

On a clean branch at the stated revision, apply the matching patch with `git apply --check` followed by `git apply`. Build using the upstream instructions and SDK version declared in that revision. The Android patch includes the server-selector support required by its RyuLDN backend. The separate [socket fixes](../../server/ryuldn/ryu-client-sockets.patch) are needed alongside these patches for the full UDP/TCP relay fixes. The patches can also be regenerated from this directory with:

```sh
python3 prepare.py ryubing --checkout /path/to/Ryubing --output /tmp/ryubing-roomcircuit.patch
python3 prepare.py kenji --checkout /path/to/Kenji-NX --output /tmp/kenji-roomcircuit.patch
python3 prepare.py kenji-android --checkout /path/to/Kenji-NX --output /tmp/kenji-android-roomcircuit.patch
```

Add `--include-sockets` if generating a single test-build patch containing the separately reviewable socket changes.

## Connect in the emulator

Enter the relay's HTTPS origin, such as `https://rooms.example.net`. For desktop LAN testing, an IPv4 private address or localhost over HTTP is supported. Android honors the app's network security policy, so use HTTPS in builds where cleartext is disabled.

**Use this relay** is optional. It is shown when discovery reports the RyuLDN listener on TCP 30456. The button saves the relay host and selects relayed RyuLDN operation. On desktop it requires closing the current game first. On Android it saves the relay selection for the next game launch. Start the same game and choose the game's **Local Wireless** menu to enter the room.

Followed events are local to this app. The panel checks them periodically while open. Switch-side alerts and emulator game ports remain independent of panel traffic.

## Verify

The patch generator checks application to the exact upstream bytes in a temporary git tree. The test project compiles the production C# client and constructs its actual Avalonia panel. The program can also exercise the native HTTP lifecycle against an isolated relay:

```sh
dotnet run --project tests/RoomCircuit.Probe.csproj -- http://127.0.0.1:11464
dotnet restore tests/RoomCircuit.Probe.csproj -p:AvaloniaVersion=12.1.6
dotnet run --no-restore --project tests/RoomCircuit.Probe.csproj -p:AvaloniaVersion=12.1.6 -- http://127.0.0.1:11464
kotlinc android/*.kt -classpath "$ANDROID_HOME/platforms/android-37.0/android.jar" -d /tmp/roomcircuit-android-panel.jar
```

The Kotlin HTTP client can run on a JVM against the same isolated relay by compiling `RoomCircuitApi.kt` with `tests/RoomCircuitProbe.kt` and a JVM `org.json` dependency. These checks do not substitute for building the complete upstream application, signing an APK, or testing a game on physical hardware.

The isolated Avalonia and relay checks ran successfully on Intel macOS using .NET SDK 10.0.301 and the respective upstream Avalonia versions 12.1.1 and 12.1.6. The Kotlin panel compiled against SDK 37, and its HTTP client passed the live native API lifecycle. The three git patches were checked against the pinned upstream sources.
