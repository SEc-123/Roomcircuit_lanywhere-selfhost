# Run and list a LAN Play node

A LAN Play node relays the game's Local Wireless traffic. RoomCircuit's **Main server** publishes the shared list of nodes and rooms. To run your own node, you need a server reachable over public IPv4. To play, use Switches running the current RoomCircuit release and a compatible game. [RoomCircuit downloads](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/releases/latest)

| What you want | What to do |
| --- | --- |
| Play on a node with friends | Each player adds the same address and UDP port to their Switch and searches for the room in the game. The node operator can do this as soon as the server is reachable. |
| Offer a node to all RoomCircuit players | The operator requests a listing. The RoomCircuit maintainers review it, and an administrator of the public **Main server** adds the approved endpoint to the shared directory. Players with a compatible release can then see it without editing their SD cards. |

If you already have a compatible LAN Play server or its operator gave you an endpoint, start at [step 2](#2-add-the-node-to-your-switch).

## 1. Start the LAN Play server

Use a VPS or another host with inbound UDP connectivity. Both methods below run the same upstream Switch LAN Play server. Pick one for this node. [Upstream server documentation](https://github.com/spacemeowx2/switch-lan-play#server)

### Python installer

On a Debian or Ubuntu VPS with Python 3, download and run the installer. Enter the public IPv4 address or DNS hostname, a display name and the region when prompted. The script installs Docker if needed, starts the relay and writes `roomcircuit-node.ini` in the current directory. [View the installer](../tools/setup_lanplay_node.py)

```sh
curl -fsSLO https://raw.githubusercontent.com/SEc-123/Roomcircuit_lanywhere-selfhost/main/tools/setup_lanplay_node.py
sudo python3 setup_lanplay_node.py
```

### Docker Compose

On the VPS with Docker Compose installed, save the Compose file as `compose.yaml` and run the command from its folder. Use the example in step 2 to create the Switch configuration. [View the Compose file](../deploy/lanplay-node/compose.yaml)

```sh
curl -fsSLO https://raw.githubusercontent.com/SEc-123/Roomcircuit_lanywhere-selfhost/main/deploy/lanplay-node/compose.yaml
docker compose up -d
```

### Open the game port and check status

Allow inbound **UDP 11451** in the VPS provider's firewall and any host firewall that filters Docker traffic. The Python installer adds a UDP rule when UFW is active. For a home server, forward the UDP port on the router as well. RoomCircuit's node settings take an address and port, and players with network access to that port can use the relay.

Both setups bind the TCP status monitor to the VPS's loopback address. On the VPS, this command returns the current number of connected LAN Play clients. An `online` count of zero is normal before any clients connect. Confirm public UDP reachability from a Switch in step 3.

```sh
curl -fsS http://127.0.0.1:11451/info
```

The Python installer also supports `python3 setup_lanplay_node.py --status`. To use a different port with Python, pass `--port` with that port to both installation and status commands. With Compose, change the host port in the two `ports` mappings and keep the container port at 11451. Use the new host port in the firewall, status URL and Switch configuration.

## 2. Add the node to your Switch

On each Switch's SD card, create or edit `config/RoomCircuit/nodes.ini` as UTF-8 plain text. Append the `[node]` block from the generated `roomcircuit-node.ini` or use the example below with your real endpoint. Preserve the entries already in the file. Each additional node starts with its own `[node]` line. The Switch supports up to seven local entries.

```ini
[node]
name=Amsterdam
host=eu-lanplay.example.net
port=11451
region=europe
```

| Entry | Purpose | Example |
| --- | --- | --- |
| `[node]` | Starts one node entry | Repeat `[node]` before a second endpoint |
| `name` | Required label in **Nodes**, up to 47 UTF-8 bytes | `name=Amsterdam` |
| `host` | Required public IPv4 address or hostname that resolves to one | `host=eu-lanplay.example.net` |
| `port` | Required UDP port published by the relay | `port=11451` |
| `region` | Optional filter label that defaults to `unknown`. Allowed values are `unknown`, `asia`, `europe`, `americas`, `oceania` and `africa` | `region=europe` |

The sample hostname is a placeholder. Use the LAN Play relay endpoint here. RoomCircuit reads its **Main server** settings separately. If two versions of `nodes.ini` conflict after an upgrade, back up both, keep the intended entries in `config/RoomCircuit/nodes.ini`, move the old file aside and reopen **Nodes**.

The file configures the RoomCircuit Switch client. Emulator and mobile clients use their own settings. [Crossplay preview](CROSSPLAY.md)

## 3. Check hosting and joining

1. Open RoomCircuit from **Album** and press **Y** in the lobby to open **Nodes**. Find the display name you set. Press **X** to refresh measurements, wait for the node to respond, then select it with **A**. The lobby's **Hosting line** should show the selected node.
2. On the host's Switch, press **+** to close RoomCircuit, then return to the game and create a room in **Local Wireless**. Use the same game and update on both consoles.
3. A friend on another network adds and selects the same hostname and UDP port, then searches for the room in the game's **Local Wireless** menu. Confirm they can join and play. If the node test fails, verify the public IPv4 address, port and firewall before retrying from the Switch.

## 4. Request a public listing

When the node works, the operator can ask to make it available to other RoomCircuit players. If the node is part of the existing discussion, reply in Issue #6. For another node, open a new issue titled **List a LAN Play node**. [Issue #6](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/issues/6) · [Open a new issue](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/issues/new)

Paste the real `[node]` block from step 2, confirm you operate the endpoint and want it listed publicly, and mention the game and result of your join check. The issue only needs the public endpoint details and the test result.

The GitHub repository is maintained under **SEc-123**. The maintainers review the endpoint's reachability and LAN Play compatibility. An operator with access to RoomCircuit's public **Main server** registers an approved node and confirms when it is available. A comment starts the review. Editing a Switch SD card adds a node to that Switch.

## 5. Confirm the public listing

After the maintainer confirms the listing, reopen RoomCircuit and open **Nodes** again. Look for the approved node on a Switch that has no local entry for it. The official directory uses the registered name and identity when the hostname and port already match an entry on your SD card.

Select the node and open a compatible public **Local Wireless** room in the game. Refresh the unified lobby from another Switch, check that the room is listed under the shared node, then select the room and join it in the game. The list shows available rooms from the node. An idle node can appear in **Nodes** with no rooms in the lobby.

## For RoomCircuit maintainers

Handle approved listings on the RoomCircuit main relay. In its existing `/etc/switch-online/relay.env`, the registry path is `ROOMCIRCUIT_CUSTOM_NODES=/var/lib/switch-online/custom-nodes.json`. Enable the LAN Play upstream integration with `LANYWHERE_ENABLE_LANPLAY_UPSTREAM=1` if needed, retaining the existing provider allowlist. The standalone node operator uses the server from step 1.

1. Review the public endpoint and inspect the existing registry and its identity ledger. Keep an endpoint's existing ID. For a new endpoint, pick an unassigned ID from 13–23. The ledger keeps prior assignments, including disabled ones. [Registry manager](../deploy/manage-custom-nodes.cjs)
2. During an idle maintenance window, back up the registry, its identity ledger and the relay configuration. Add the approved endpoint with the manager, confirm the entry with `list`, and apply the configuration by restarting `switch-online.service`. Lowercase hostnames and use numeric regions in the command. The codes are 0 unknown, 1 Asia, 2 Europe, 3 Americas, 4 Oceania and 5 Africa.
3. Check that the relay is healthy and the node is visible to a Switch without a local entry. Verify room discovery and joining as in step 5, then reply to the operator with the outcome. If a listing needs to be taken offline, run the manager's `disable ID` command and apply the change during a maintenance window. The ID stays assigned to that endpoint.

On the main relay, replace the sample endpoint and ID with the values reviewed above.

```sh
sudo node /opt/switch-online/current/manage-custom-nodes.cjs list
sudo node /opt/switch-online/current/manage-custom-nodes.cjs add 13 eu-lanplay.example.net 11451 2 "Amsterdam"
sudo node /opt/switch-online/current/manage-custom-nodes.cjs list
sudo node /opt/switch-online/current/validate-config.cjs /etc/switch-online/relay.env
sudo systemctl restart switch-online.service
sudo node /opt/switch-online/current/healthcheck.cjs --once
```

The relay loads the registry at startup and validates public IPv4 destinations before use. If activation fails, restore the previous registry and relay configuration and verify service health. For registry automation, the manager writes an atomic JSON file in the format below. Retain existing entries and the identity ledger.

```json
{"version":1,"nodes":[{"id":13,"host":"eu-lanplay.example.net","port":11451,"region":2,"name":"Amsterdam","enabled":true}]}
```

## Tuner or Ultrahand package setup

A package author can supply a local default by placing a prepared `RoomCircuit-nodes.ini` beside the package commands. The example copies it on first setup when no `nodes.ini` exists. To add entries to an existing file, use a parser that preserves every `[node]` block and merges by hostname and port. Reopen **Nodes** after saving. Verify the commands against the target Tuner or Ultrahand version before distribution. [Ultrahand command reference](https://github.com/ppkantorski/Ultrahand-Overlay/wiki/Command-Reference)

```ini
[Install RoomCircuit node defaults]
try:
!path_exists /config/RoomCircuit/nodes.ini
mkdir /config/RoomCircuit
copy ./RoomCircuit-nodes.ini /config/RoomCircuit/nodes.ini
```
