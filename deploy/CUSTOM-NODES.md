# Shared custom LAN Play nodes

RoomCircuit clients can read locally configured nodes. To show a custom node's rooms to all compatible clients in the unified lobby, an administrator must register the same host and port on the main relay.

Set this line in `/etc/switch-online/relay.env`:

```text
ROOMCIRCUIT_CUSTOM_NODES=/var/lib/switch-online/custom-nodes.json
```

As root, create or update a slot using the installed manager:

```sh
node /opt/switch-online/current/manage-custom-nodes.cjs add 13 lanplay.example.org 11451 2 "Russia"
node /opt/switch-online/current/manage-custom-nodes.cjs list
node /opt/switch-online/current/manage-custom-nodes.cjs disable 13
```

The arguments are slot ID, hostname or IPv4, UDP port, existing RoomCircuit region ID, and display name. Region IDs are 0 unknown, 1 Asia, 2 Europe, 3 Americas, 4 Oceania and 5 Africa. Hostnames must be lowercase. The list contains public endpoints only. Slots 13–23 are reserved for shared custom nodes and their host/port identity is permanent. Disabling or removing an entry never frees its slot for a different endpoint. Restart the relay during an idle maintenance window to apply changes. Existing 13 built-in nodes are unchanged.

For automation, the manager writes an ordinary JSON file using an atomic rename and preserves other entries. A script can also safely generate the same format:

```json
{"version":1,"nodes":[{"id":13,"host":"lanplay.example.org","port":11451,"region":2,"name":"Russia","enabled":true}]}
```

The relay checks every registry entry on startup and refuses changed or reused historical identities. Its separate identity ledger and the configuration are included in state backup. An invalid file causes startup to fail with an explicit error rather than being interpreted as an empty allowlist. The relay rejects private, loopback, link-local, benchmark, multicast and other reserved addresses both for literals and after DNS resolution. A realm pins its verified IPv4 for its lifetime and resolves again on the next start. The V2 directory sends that same endpoint in each room locator, so guests use the server where that room was actually found. A saved pagination snapshot retains its original endpoint even when DNS later changes. It never accepts node addresses submitted by network clients.

Newer clients read the shared directory separately from the fixed legacy catalog. Older clients keep their existing catalog, room pages and friend measurement protocol. A locally configured endpoint is available to that local player even before an administrator registers it; an unregistered node's rooms cannot be advertised by the shared directory.

## Tuner / Ultrahand example

A Tuner package may include a prepared `RoomCircuit-nodes.ini` beside its package commands. This example copies it only on first setup and leaves an existing file untouched:

```ini
[Install RoomCircuit node defaults]
try:
!path_exists /config/RoomCircuit/nodes.ini
mkdir /config/RoomCircuit
copy ./RoomCircuit-nodes.ini /config/RoomCircuit/nodes.ini
```

To add entries to an existing file, use a parser that preserves every `[node]` section and merges by hostname and port. Reopen **Nodes** after saving. This is a package-author example; verify it with the target Tuner/Ultrahand version before distribution. Command syntax follows the [Ultrahand command reference](https://github.com/ppkantorski/Ultrahand-Overlay/wiki/Command-Reference).

## Tuner / Ultrahand 示例

整合包可在脚本旁提供 `RoomCircuit-nodes.ini`。上述命令只在首次配置时复制示例文件，已有配置不会被覆盖。要向已有文件追加节点，应使用能保留全部 `[node]` 段、按主机名和端口合并的解析工具。保存后重新打开「节点」。发布前请在目标 Tuner/Ultrahand 版本上验证脚本。
