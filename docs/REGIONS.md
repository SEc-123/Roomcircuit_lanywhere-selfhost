# Nodes and compatibility · 节点与兼容范围

## Choose and join

- **Host alone.** Press **Y** in the lobby, choose a node, and confirm with **A**. Check **Hosting line** before creating a Local Wireless room in the game. The new room uses that node, including a third-party node if selected. **R** filters regions, and **X** refreshes measurements. Main server is the initial choice.
- **Recommended:** up to three nodes, based on this Switch's reply success, latency and variation. Regions limit the recommendations. An empty region has no recommendation. Your future guests may have different results.
- **Join:** the unified lobby lists rooms from available sources. **ZL+ZR** filters game and region. **A** checks your connection before selecting that room's actual node. A weak result offers Continue or Back. Unknown game IDs remain visible under All games.
- **Friends, optional:** **L** in the picker creates or joins a six-character test code. Keep the node screen open; the comparison considers every participant's current measurements. The host chooses a node, then hosts normally. The code lasts ten minutes and does not create a game room.

Measurements run on the Switch. TCP and UDP are labeled separately; successful probe replies are not a measurement of game packet loss. Results expire after a minute and refresh after a network change or resume. A node can work for one player and fail for another. No latency improvement is promised without comparing actual play with the same players, game and version.

A direct room's gameplay uses its public node. Main server still supplies the unified directory and shared tests. Older rooms without usable direct session identity keep compatibility forwarding. The check then says **Your connection to Main server gateway**: it measures only that leg, not the onward relay. Original LANywhere legacy rooms can use this path.

An active room keeps its node. Recovery retries that node; it does not move players to another server. Longer outages, a departed host or an expired room can require joining again. Appointment alerts use **Main server**. The lobby shows the game name, ownership and password choices once it has matched a RoomCircuit host to its current direct room. Set and enter the actual password in the game.

## Directory

The shared [catalog](../common/node-catalog.json) records stable IDs, endpoints, protocols and region evidence. Region labels follow the cited directory, not measured geography. “Node” and “Rust” identify relay implementations, not the game's standard/fork client protocol. These are configured destinations, not a promise that every endpoint is online or that the directory contains every public server.

| Node | Endpoint | Region basis |
| --- | --- | --- |
| Main server | `49.233.147.207:11460` | China — operator-provided deployment |
| LANywhere Community | `129.38.114.182:11460` | Unverified |
| Jaysea · Node / Rust | `switch.jayseateam.nl:11453` / `:11451` | Netherlands — directory |
| R3ps4j · Rust / Node | `switch.r3ps4j.nl:11451` / `:11452` | Netherlands — directory |
| Muitxobem | `muitxobem-lanplay.ddns.net:11451` | Brazil — directory |
| Mainbareng | `switch.mainbareng.web.id:11451` | Indonesia — directory |
| Servegame | `switch.servegame.com:11451` | Germany — directory |
| Jax | `srv.jaxlewis.top:11451` | China |
| Gugu | `gugu.mulaosi.cn:11451` | China |
| Olunira | `olunira.fun:11451` | China |
| 47.102.121.212 | `47.102.121.212:11451` | China |

Regions are based on the [LAN Play directory](https://github.com/GreatWizard/lan-play-status/blob/master/src/data/servers.json) and [APNIC address registrations](https://www.apnic.net/about-apnic/whois_search/). The **China** filter is available in RoomCircuit v0.9.3 and later.

Servegame is disabled in this release: probes responded, but discovery/admission failed. [Per-node protocol checks](distribution/PUBLIC-NODES-0.8.0.md)

## Compatibility

The direct Switch LAN Play adapter handles the supported standard ldn_mitm and fork formats. The first guest accepted by the game locks that room to one format; incompatible later guests are rejected. Reconnecting keeps the lock. Recreating the room clears it. Use the same LANywhere version to avoid mixing client formats. Standard game streams use TCP. The fork format carries streams over its existing UDP protocol, which cannot provide equivalent reliability under packet loss or reordering; its individual game UDP payloads are limited to 1472 bytes. Relay reachability does not establish compatibility with every game or client fork. Use matching game versions.

The original LANywhere node is a separate legacy protocol: its UDP game datagrams are limited to 1472 bytes and it lacks Main server's game TCP extension. Games needing those extensions should use Main server. Native Nintendo Online services and games without a compatible Local Wireless mode are outside this feature.

See [validation and unfinished hardware checks](distribution/VALIDATION.md). Public-node gameplay between two physical Switch consoles and a measured regional latency improvement remain unverified.

## 中文

- **单人建房。** 大厅按 **Y** 选节点，按 **A** 确认。确认底部的「建房线路」后，再回游戏在「本地无线」里建房。新房间使用当前线路，选了第三方节点就用该节点建房。**R** 筛地区，**X** 重测。初始线路是主服务器。
- **推荐依据：** 按你这台 Switch 的回应成功率、延迟和波动，最多推荐三个；选地区后只在该地区推荐。没有节点的地区不推荐。后来加入者的网络可能不同。
- **加入：** 统一大厅汇总可用来源的房间，**ZL+ZR** 筛游戏和地区，**A** 检查你到房间节点的连接。连接较差可继续或返回。未识别游戏仍在「所有游戏」里显示。
- **一起测速可选：** 节点页 **L** 创建或输入六位临时码，留在节点页查看全体参与者的结果；建房者选线路后照常建房。码十分钟有效，不会替你创建游戏房间。

测速在 Switch 上进行，TCP/UDP 分开标注；探测回应成功率不是游戏丢包率，数值也不是游戏内延迟。结果一分钟过期，换网络或恢复前台后重测。只有同组玩家、同款同版本游戏的对照实测，才能判断延迟是否改善。

直连房间的对局走实际公共节点，主服务器提供目录与一起测速。缺少可用直连会话信息的旧房间保留兼容转发，包括部分原版 LANywhere 房间。检查页会显示「你到主服务器转接的延迟」，不包括后续转发耗时。

房间创建后固定节点，断线只重试原节点，不会自动换服。断网较久、房主离开或房间过期时需重新加入。约局提醒用「主服务器」。大厅确认当前第三方直连房间的房主后，会同步游戏名、标出「我的房间」，并提供密码分享或隐藏。实际密码在游戏里设置和输入。

本版 Servegame 探测有响应但建房/加入失败，已停用。其余 10 个 LAN Play 节点通过公网协议测试。

上表共 13 个配置入口。荷兰、巴西、印度尼西亚、德国来自目录标注，主服务器所在地由运营者提供；其他未核实，不凭域名或语言猜地区。入口不等于当前在线，也不代表收齐了所有服务器。

已适配标准 ldn_mitm 和 fork 格式。首个被游戏接受的访客会锁定该房间的协议，后续不兼容客户端会被拒绝；断线恢复不解锁，重建房间才清除。建议大家使用同版 LANywhere。标准游戏流使用 TCP；fork 沿用其 UDP 流格式，丢包、乱序时不能保证相同可靠性，单个游戏 UDP 数据最多 1472 字节。原版 LANywhere 另有 1472 字节 UDP 限制，且没有主服务器的游戏 TCP 扩展；需要这些能力的游戏请用主服务器。节点可达不等于所有游戏或 fork 均兼容，也不替代任天堂官方在线服务。

双 Switch 公共节点实机游玩及地区延迟改善仍待验证，详见[验证记录](distribution/VALIDATION.md)。
