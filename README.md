# RoomCircuit (formerly LANywhere Self-host)

RoomCircuit brings compatible Nintendo Switch games online through their built-in **Local Wireless** mode. Host in the game as you would with someone nearby, and players on other networks can find and join your room.

[Download the latest release](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest) · [Install](#install) · [Play](#play) · [Update](#update)

## What you can do

- **Browse and join rooms.** See public rooms across available nodes and filter by game or region.
- **Choose where to host.** Test built-in, administrator-published and locally configured LAN Play nodes. Compare the 13 built-in nodes with friends using a shared test code.
- **Manage room passwords.** Share your in-game password in the lobby or keep it private.
- **Schedule a session.** Agree on a time with other players so nobody has to leave a game room open while waiting. Players can mark themselves ready and find the room when it opens.

## Install

You need a Switch running Atmosphère, an internet connection, and a game with a compatible Local Wireless mode. Use the same game version as the other players.

1. Download the **full SD ZIP** (`switch-online-*-sd.zip`) from the [latest release](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest).
2. Extract it on a computer. Merge the ZIP's `atmosphere/` and `switch/` folders, and `config/` if present, into the root of your SD card. This installs RoomCircuit's version of the `ldn_mitm` module.
3. Safely eject the SD card and restart Atmosphère. Open **RoomCircuit** from **Album** or its overlay. The app is in `switch/RoomCircuit/`, the overlay in `switch/.overlays/`, and settings in `config/RoomCircuit/`.

## Play

For emulator and mobile interoperability, see the [crossplay preview](docs/CROSSPLAY.md).

### Host a room

1. From the lobby, press **Y** to open **Nodes**. Choose a node and press **A**. In the picker, press **R** to filter by region, **X** to retest, or **L** to test with friends.
2. Check **Hosting line** at the bottom of the lobby. Press **+** to close RoomCircuit, then return to the game and create a room in **Local Wireless**. The new room uses the selected node.
3. Set the room password in the game, then reopen RoomCircuit. Once the lobby shows **My room** and the password action is available, press **Y** and choose **Share in lobby** or **Keep password hidden**.

You can also choose the hosting node from the RoomCircuit overlay in Ultrahand. Leave the current local-wireless session before switching nodes. The overlay follows Ultrahand's theme, side and sound settings. It needs Ultrahand and its loader; the Album app works separately.

### Join a room

1. Use **ZL + ZR** to filter the lobby by game and region. Select a room and press **A**. RoomCircuit checks your connection and selects the room's node.
2. Open or return to the game, choose **Local Wireless**, and search for the room. Enter the game's password when prompted.

If the game uses link codes instead of a room list, choose the same node, enter the same code in the game, and start local matchmaking on both consoles.

### LAN Play nodes

A LAN Play node is a public service that relays Local Wireless traffic between players over the internet. Its hostname and port identify an existing service. A `nodes.ini` entry does not create or install a server.

**Using RoomCircuit's built-in nodes needs no extra computer, server software or server hardware.** Install RoomCircuit on the Switch, connect it to the internet, and select a node in **Nodes**. To use another operator's node, ask that operator for its public hostname or IPv4 address, UDP port and region.

The RoomCircuit **Main server** is not a LAN Play node. Do not copy the Main server address from `config.ini` into `nodes.ini`.

#### Add a node on Switch

On the SD card, create or edit `config/RoomCircuit/nodes.ini`. Add one `[node]` block for each endpoint. This example uses reserved example hostnames and will not connect until you replace them with real values:

```ini
[node]
name=Amsterdam
host=eu-lanplay.example.net
port=11451
region=europe

[node]
name=North America test
host=us-lanplay.example.net
port=11451
region=americas
```

| Entry | What it controls | Example |
| --- | --- | --- |
| `[node]` | Starts one node entry. Repeat it for every endpoint. | A second block starts another node. |
| `name` | Required. The label shown in the node list. It can describe the operator or location. | `name=Amsterdam` |
| `host` | Required. The server's public IPv4 address or DNS hostname, supplied by its operator. | `host=eu-lanplay.example.net` |
| `port` | Required. The node's UDP listening port. Use the exact port supplied by the operator. | `port=11451` |
| `region` | Optional region label used by the list filter. It does not move the server or guarantee lower latency. Omit it to use `unknown`. | `region=europe` |

Allowed regions are `unknown`, `asia`, `europe`, `americas`, `oceania` and `africa`. Up to seven local nodes are supported.

After saving the file, reopen **Nodes**. Select the endpoint and press **X** to test it. Choose it with **A**, check **Hosting line** in the lobby, then return to the game and create or search for a room in **Local Wireless**.

A custom node that is not published in RoomCircuit's shared directory is local to each player. For that node, everyone must configure and select the same host and port, then find the room from the game's Local Wireless search. To show its rooms in the shared lobby, the node operator must ask a RoomCircuit administrator to register that endpoint. See the [administrator setup](deploy/CUSTOM-NODES.md).

#### One-click self-host setup

The standard LAN Play server is [spacemeowx2/switch-lan-play](https://github.com/spacemeowx2/switch-lan-play#server). RoomCircuit implements its LAN Play wire protocol. On a Debian or Ubuntu VPS, download the [RoomCircuit setup script](tools/setup_lanplay_node.py) and run:

```sh
curl -fsSLO https://raw.githubusercontent.com/SEc-123/Roomcircuit_lanywhere-selfhost/main/tools/setup_lanplay_node.py
sudo python3 setup_lanplay_node.py
```

The script installs Docker if needed, starts the server, asks for the public node address, display name and region, then prints and saves a `nodes.ini` entry. Copy that entry into `config/RoomCircuit/nodes.ini` on each Switch. It adds an inbound UDP rule if UFW is already active. Open UDP **11451** in the VPS provider's cloud firewall too.

To see how many LAN Play clients are connected, SSH into the VPS and run `sudo python3 setup_lanplay_node.py --status`. This monitor reports a count only and is bound to localhost. The server has no per-user authentication in the RoomCircuit node settings, so anyone who can reach the UDP port can use it.

RoomCircuit supplies the Switch client; players do not need the upstream desktop `lan-play` client. No special physical hardware is required. A home server needs a public inbound route and router/firewall configuration. The script does not change cloud-provider firewall settings.

`nodes.ini` configures the Switch RoomCircuit client only. Android, desktop and other mobile clients use their own network settings and may require a compatible client build. See the [crossplay preview](docs/CROSSPLAY.md) for the client-specific setup currently documented; adding a Switch node entry does not configure those clients.

If the old and new `nodes.ini` files changed differently during an upgrade, back up both, keep the intended entries in `config/RoomCircuit/nodes.ini`, move the old file aside, and reopen **Nodes**.

### Schedule a session

Open **Appointments** with **−** from the lobby. Press **L** to create a session and **A** to join or leave one. From 30 minutes before the start, press **Y** to mark yourself ready. Press **X** to find rooms near the start time. Game-time room alerts require **Main server** and Ultrahand with API notifications enabled.

On a phone or computer, open the relay’s [shared appointments panel](docs/WEB-APPOINTMENTS.md) to schedule, share, get ready and find open rooms with Switch players.

Supported emulator development builds can open the same appointments inside the app. See [native emulator integrations](docs/NATIVE-INTEGRATIONS.md).

Press **→** from the room list to open the in-app guide. See [node routing and game compatibility](docs/REGIONS.md) for details.

## Update

### In-app update (v0.8.8 or later)

**v0.8.8** is the recommended starting version for in-app updates with the standard RoomCircuit build. The installed version appears beside the app name.

1. Open RoomCircuit and press **LS** (click the left stick) to check for updates. **Update available** is highlighted when a newer version is detected.
2. Close any running game from **HOME** before installing the update.
3. Select **Download and install**. Keep the Switch on until **Restart required** appears, then restart into Atmosphère.

### Manual update (any version)

Use the [full SD ZIP](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest) for a first install, to upgrade from a version before **v0.8.8**, or if the in-app update fails. Follow the [installation steps](#install) and overwrite matching files. Keep the existing `lanywhere/` and `switch/RoomCircuit/` data until migration to `config/RoomCircuit/` completes. For a custom server build, use its matching SD ZIP.

## Build and self-host

- Build with `bash scripts/build-container.sh --embedded` using the [pinned toolchain](scripts/toolchain/README.md).
- Start the relay with `npm start` on Node.js 22 or later. See the [deployment guide](docs/DEPLOY.zh-CN.md) and [connection profile](common/relay_profile.h).
- See [update publishing](docs/distribution/ONLINE-UPDATES.md#publishing) and [pack and store integration](docs/distribution/INTEGRATORS.md).

## Credits

Based on [LANywhere by augustocco](https://github.com/augustocco/lanywhere) and [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm). [Third-party notices](third_party/README.md) · [GPL-2.0-only](LICENSE).

