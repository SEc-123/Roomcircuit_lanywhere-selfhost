# RoomCircuit (formerly LANywhere Self-host)

RoomCircuit brings **Local Wireless multiplayer** for compatible Nintendo Switch games online. Its main client runs on **Atmosphère** in both **emuMMC** and **sysMMC** modes. Players can find rooms across different servers, choose a faster route, and schedule a game before anyone needs to open a room.

[Download the latest release](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest) · [Install](#install) · [Play](#play) · [Update](#update)

## Supported clients

| Platform | Client |
| --- | --- |
| Nintendo Switch | RoomCircuit on Atmosphère (emuMMC or sysMMC) |
| Desktop | Eden, compatible Yuzu builds, and supported Ryujinx-family builds such as Ryubing and Kenji-NX |
| Android | Eden and supported Kenji-NX builds |
| iPhone | NX Pocket and patched MeloNX with the LAN Play gateway setup |
| Mac | NX Pocket with the LAN Play gateway setup |

Emulator crossplay is a preview. Use the same compatible game and update on each device, and follow the [crossplay setup](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/docs/CROSSPLAY.md) for your client.

## What you can do

- **Browse and join rooms.** See public rooms across available nodes and filter by game or region.
- **Choose where to host.** Measure latency to available nodes, or compare routes with friends before you host. Use the **China** filter to find Chinese nodes.
- **Manage room passwords.** Share your in-game password in the lobby or keep it private.
- **Schedule a session.** Agree on a time with other players so nobody has to leave a game room open while waiting. Players can mark themselves ready and find the room when it opens.

## Install

You need a Switch running Atmosphère, an internet connection, and a game with a compatible Local Wireless mode. Use the same game version as the other players.

1. Download the **full SD ZIP** (`switch-online-*-sd.zip`) from the [latest release](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest).
2. Extract it on a computer. Merge the ZIP's `atmosphere/` and `switch/` folders, and `config/` if present, into the root of your SD card. This installs RoomCircuit's version of the `ldn_mitm` module.
3. Safely eject the SD card and restart Atmosphère. Open **RoomCircuit** from **Album** or its overlay. The app is in `switch/RoomCircuit/`, the overlay in `switch/.overlays/`, and settings in `config/RoomCircuit/`.

## Play

### Host a room

1. Press **Y** next to **Hosting line** at the bottom of the lobby. Choose a node and press **A**. In the picker, press **R** to filter by region, **X** to retest, or **L** to test with friends.
2. Press **+** to return to the game and create a room in **Local Wireless**.
3. Set the room password in the game, then reopen RoomCircuit. Press **Y** next to **My room** and choose **Share in lobby** or **Keep password hidden**.

You can also choose the hosting node from the RoomCircuit overlay in Ultrahand. Leave the current local-wireless session before switching nodes. The overlay follows Ultrahand's theme, side and sound settings. It needs Ultrahand and its loader; the Album app works separately.

### Join a room

1. Press **ZL + ZR** by the room list to filter by game and region. Select a room and press **A**. RoomCircuit checks your connection and selects the room's node.
2. Open or return to the game, choose **Local Wireless**, and search for the room. Enter the game's password when prompted.

If the game uses link codes instead of a room list, choose the same node, enter the same code in the game, and start local matchmaking on both consoles.

### LAN Play nodes

<a name="running-your-own-node"></a>
<a name="one-click-self-host-setup"></a>

To run a LAN Play node and offer it as a shared hosting option, follow the [LAN Play node guide](docs/LAN-PLAY-NODES.md).

### Schedule a session

Open **Appointments** with **−** from the lobby. Press **L** to create a session and **A** to join or leave one. From 30 minutes before the start, press **Y** to mark yourself ready. Press **X** to find rooms near the start time. Game-time room alerts require **Main server** and Ultrahand with API notifications enabled.

On a phone or computer, use the [shared appointments panel](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/docs/WEB-APPOINTMENTS.md) to schedule games with Switch players.

Supported emulator builds can open the same appointments inside the app. See [native emulator integrations](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/docs/NATIVE-INTEGRATIONS.md).

Press **→** from the room list to open the in-app guide. See [node routing and game compatibility](docs/REGIONS.md) for details.

## Update

### In-app update (v0.8.8 or later)

**v0.8.8** is the recommended starting version for in-app updates with the standard RoomCircuit build. The installed version appears beside the app name.

1. Open RoomCircuit and press **LS** (click the left stick) to check for updates. **Update available** is highlighted when a newer version is detected.
2. Open **Update available** to review both versions and what's changed.
3. Close the game, select **Download and install**, then restart into the same Atmosphère mode when prompted.

### Manual update (any version)

Use the [full SD ZIP](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest) for a first install, to upgrade from a version before **v0.8.8**, or if the in-app update fails. Follow the [installation steps](#install) and overwrite matching files. Keep the existing `lanywhere/` and `switch/RoomCircuit/` data until migration to `config/RoomCircuit/` completes. For a custom server build, use its matching SD ZIP.

## Build and self-host

- Build with `bash scripts/build-container.sh --embedded` using the [pinned toolchain](scripts/toolchain/README.md).
- Start the relay with `npm start` on Node.js 22 or later. See the [deployment guide](docs/DEPLOY.zh-CN.md) and [connection profile](common/relay_profile.h).
- See [update publishing](docs/distribution/ONLINE-UPDATES.md#publishing) and [pack and store integration](docs/distribution/INTEGRATORS.md).

## Credits

Based on [LANywhere by augustocco](https://github.com/augustocco/lanywhere) and [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm). [Third-party notices](third_party/README.md) · [GPL-2.0-only](LICENSE).

