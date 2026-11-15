# Community and regional connections

## Public nodes in 0.8.0

The standard release provides on-console node measurement: lobby **Y** selects a hosting node, **R** filters regions, and **X** retests. Guests use the actual node of the room selected in the unified lobby. Direct LANywhere and supported Switch LAN Play routes keep gameplay off the central directory server; legacy rooms without usable direct identity retain explicitly labeled compatibility forwarding. See [Nodes and compatibility](../REGIONS.md) for player steps, region evidence, standard/fork limits and unfinished hardware acceptance.

The central server still provides the room directory, appointments and optional shared measurements. Public nodes are independently operated. `LANYWHERE_ENABLE_PUBLIC_UPSTREAM=1` enables the original LANywhere directory source; `LANYWHERE_ENABLE_COMMUNITY_DIRECT=1` enables its direct hosting choice. The server's catalog and the client build must agree. A configured source's availability is separate from a player's measured connection. Short outages retry the captured room endpoint; recovery never changes it to another region. Appointment alerts and lobby password sharing use Main server.

0.8.0 标准包可在大厅按 **Y** 单人测速选节点，**R** 筛地区，**X** 重测；加入时跟随房间实际节点。支持直连的房间对局不经过主服务器，缺少直连会话信息的旧房间继续兼容转发并明确标注。地区依据、协议边界和操作见[节点说明](../REGIONS.md)。下文的自管欧洲节点是额外部署选项，不是使用公共节点的前提。

## Optional self-managed regional connections

A regional build lets players choose China or Europe before hosting. The unified lobby lists open rooms from both relays. Choosing a room connects gameplay directly to its relay, including the room's access check, join and reconnection. A guest can keep a different region for the next room they host.

The default release profile uses China. Europe appears when the client build includes the Europe endpoint. The central server provides the regional room directory. The Europe server operates its own rooms, reservations, and game traffic. Configure both before distributing the regional build.

## Configure the nodes

1. Run the standard relay on an authorized Europe server. Expose its game port over TCP and UDP. Keep its state directory separate from the central relay state.
2. Set `LANYWHERE_EU_RELAY=IPV4:PORT` in the central relay's environment using the Europe server's reachable address. The central relay reads that server's public room metadata.
3. Set the same IPv4 and port in `RELAY_PROFILE_EU_IPV4` and `RELAY_PROFILE_EU_PORT` in `common/relay_profile.h`. Rebuild the Album app and sysmodule together via the normal embedded build.
4. Check the SD ZIP's app, sysmodule and overlay versions, and verify that a room created on Europe is visible in the new unified lobby. The Switch checks that the regional address advertised by the directory matches its configured endpoint.

Legacy clients use the earlier room list and continue to see their existing sources.

Appointments and appointment reminders use the central server. A room hosted in Europe appears in the main lobby. It does not generate a central appointment reminder. Use a central room if the appointment group relies on the reminder.

## Validate before enabling Europe

- Run the repository's host tests. With two independent relays, create rooms on each, confirm they appear with the correct source, and select a Europe room with the hosting preference set to China. Check that game traffic arrives on Europe and a connection loss resumes at the same node.
- Confirm public TCP and UDP reachability from both players. Compare RTT, jitter and loss for each player to both nodes, then compare actual gameplay with the same game and version.
- Check the workflows with a password-protected room, a region change while idle, and a connection in progress. Reboot and confirm the default is China. Regional selection is kept until the module restarts.
- Test on two physical Switch consoles before documenting gameplay or latency as verified for a specific region.

The central directory aggregates room listings. It does not forward game traffic between regional relays. Each room uses one relay.
