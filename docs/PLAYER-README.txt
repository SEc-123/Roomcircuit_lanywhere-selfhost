RoomCircuit

中文

让支持本地无线的 Switch 游戏跨网联机。跨节点找房、测速选线路，或提前约局。

安装
在电脑解压，把 atmosphere、switch 和存在的 config 合并到 SD 卡根目录。覆盖同名文件，保留原目录和设置。
安装会替换相同模块 ID 的 ldn_mitm。重启进入兼容的 Atmosphère，再从相册打开 RoomCircuit。
   @CONNECTION_ZH@
默认英文。点右上角或按 RS 切换中英文，设置会保留。右下角可「查看教程」。
游戏需要玩家名称时，自动使用 Switch 主机名称。
应用、浮层与设置分别位于 switch/RoomCircuit/、switch/.overlays/ 和 config/RoomCircuit/。
自定义服务器安装包保留已有配置；新旧配置冲突时不会覆盖原文件。若仍有旧入口，请选择新路径中的 RoomCircuit.nro，确认后再手动移除旧入口。
从旧版升级前备份 lanywhere/ 和 switch/RoomCircuit/；不要在在线更新尚未完成时使用其他安装器。
若迁移后后台无法加载，请保留并备份旧设置目录和 config/RoomCircuit/ 后再排查。
若降级后提示旧设置已变更，请备份并对比 switch/RoomCircuit/data/ 和 config/RoomCircuit/data/，再决定保留哪些数据。

在线更新
标题旁显示当前版本。有新版时，顶部高亮提示。按 LS 可手动检查更新。关闭游戏后安装，完成后重启虚拟系统。
安装中断时，从自制软件菜单打开 RoomCircuit Recovery。

建房与加入
• 单人建房。大厅按 Y 选节点，按 A 确认。看一眼底部「建房线路」，再按 + 回游戏，在「本地无线」里建房。新房间会用这条线路，选了第三方节点就用该节点建房。节点页按 R 筛地区，X 重新测速。
• 重启后保留所选建房节点。在 Ultrahand 打开 RoomCircuit，选「建房节点」即可切换；请先退出联机房间。
• 推荐。最多三个，根据你这台 Switch 的网络产生。TCP/UDP 是测速方式，不是游戏内延迟，也不代表其他玩家的网络。地区未核实会明确标注。
• 加入。左键浏览游戏，A 筛选，右键回房间。ZL+ZR 可筛游戏和地区。A 选房，查看你到房间节点的连接，再打开游戏搜索。连接较差时可选择继续或返回。
• 一起测速。节点页按 L 创建或输入临时码，共同推荐当前覆盖 13 个内置节点。自定义节点可单独测速。
• 对局线路。直连房走房间实际公共节点；旧版「社区 · LANywhere」等房间可能经主服务器兼容转发，连接检查会明确标注。断线只尝试原节点，恢复失败需重新加入。
• 密码与提醒。在游戏内设密码。自己的房间显示「我的房间」后，可在大厅分享或隐藏游戏密码。第三方直连时仍在游戏里验密码。约局提醒使用主服务器。
• 兼容范围。标准和 fork 协议不能混房；首个被游戏接受的访客确定协议，重建房间可更换。建议同版 RoomCircuit、同版游戏。节点可达不代表每款游戏已实测，详见项目的节点说明与验证记录。

自定义 LAN Play 节点（Switch）
LAN Play 节点是可从公网访问的服务端，为异地玩家转发本地无线游戏数据。使用 RoomCircuit 预置节点不需要额外电脑、服务器软件或服务器硬件；使用别人运营的节点，也只需对方提供地址和端口。
RoomCircuit「主服务器」不是 LAN Play 节点，不要把 config.ini 中的主服务器地址填进 nodes.ini。

在 SD 卡的 config/RoomCircuit/nodes.ini 添加节点。每个 [node] 区块代表一个地址，可以重复多个：
[node]
name=Amsterdam
host=eu-lanplay.example.net
port=11451
region=europe

[node]
name=备用节点
host=backup.example.net
port=11451
region=unknown

[node] 开始一条节点记录，添加第二条时再次写 [node]。
name、host、port 为必填：name 是节点列表名称，如 Amsterdam；host 是运营者提供的公网 IPv4 或域名，如 eu-lanplay.example.net；port 是服务器 UDP 监听端口，请使用运营者给出的值（11451 仅为示例）。
region 是可选的地区筛选标签，可填 unknown、asia、europe、americas、oceania 或 africa。它只用于分类，不会改变线路或保证延迟。省略时按 unknown 处理。
上面的域名均为示例，不能直接连接。最多可配置 7 个本机节点。

保存后重新打开节点页。选中节点按 X 测试，按 A 选择。两名玩家使用未收录节点时，双方都需配置并选择相同的 host 和 port，再在游戏内搜索本地无线房间。只有管理员将节点登记到 RoomCircuit 共享目录后，该节点的房间才会出现在统一大厅。

自己运营新节点才需要额外服务器。标准服务端是 https://github.com/spacemeowx2/switch-lan-play ，RoomCircuit 已实现它使用的 LAN Play 协议。
Debian/Ubuntu VPS 一键部署：下载 tools/setup_lanplay_node.py，然后运行：
curl -fsSLO https://raw.githubusercontent.com/SEc-123/Roomcircuit_lanywhere-selfhost/main/tools/setup_lanplay_node.py
sudo python3 setup_lanplay_node.py
脚本会安装 Docker（如需要）、启动服务、询问节点公网地址、名称和地区，并输出及保存 nodes.ini 配置。将配置复制到每台 Switch 的 config/RoomCircuit/nodes.ini。若 UFW 已启用，脚本会放行 UDP 11451；云厂商防火墙仍需手动开放该端口。
在 VPS 运行 sudo python3 setup_lanplay_node.py --status 可查看连接客户端数量。状态接口只返回数量，且仅限 VPS 本机访问。
已安装 Docker Compose 时，也可使用 https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/deploy/lanplay-node/compose.yaml 并运行 docker compose up -d。在 VPS 上运行 curl http://127.0.0.1:11451/info 可查看连接客户端数。选择脚本或 Compose 其中一种部署方式。
RoomCircuit 的节点配置不支持账号密码，因此能访问 UDP 端口的设备都可以使用该节点。RoomCircuit 已包含 Switch 客户端，无需在电脑上运行上游 lan-play 客户端。家用主机还需公网入站线路和路由器端口配置。

此文件只配置 Atmosphère 上的 RoomCircuit。Android、桌面模拟器和其他移动端需按各自客户端设置，且需兼容的客户端构建；见 Crossplay preview。

若升级时新旧 nodes.ini 都有修改且内容不一致，请备份两份，将需要的节点保留在 config/RoomCircuit/nodes.ini，移走旧文件后重新打开节点页。

约局与双人配对
• 在大厅按 − 或点「约局」安排游戏。约局页按 A 参加或退出、L 发起、Y 准备、X 找房，按 B 回大厅。
• 用「主服务器」开房可发出约局提醒。游戏内弹窗需要 Ultrahand 启用 API 通知。
• 宝可梦连接交换等双人配对，两边在游戏里选本地通信，输入相同连接码并开始搜索。

English

Play compatible Switch Local Wireless games online. Find rooms across nodes, test your connection, or schedule a session.

Install
Extract on a computer and merge atmosphere, switch and config (if included) into the SD card root.
Overwrite matching files, but do not delete or replace the whole existing atmosphere folder.
This replaces an existing ldn_mitm with the same module ID. Restart into compatible Atmosphère and open RoomCircuit from Album.
   @CONNECTION_EN@
Use the top-right language control or press the right stick (RS) to switch languages. Your selection is remembered.
Choose View guide at the bottom right for help.
Games that need a player name use your Switch name automatically.
The app, overlay and settings are in switch/RoomCircuit/, switch/.overlays/ and config/RoomCircuit/.
Custom-server packages preserve existing settings and do not overwrite conflicting configurations. If an old entry remains, use RoomCircuit.nro at the new path and check the old entry before removing it.
Before upgrading an older installation, back up lanywhere/ and switch/RoomCircuit/. Do not run another installer while an online update is pending.
If the module cannot load after migration, keep and back up the old settings folders and config/RoomCircuit/ before troubleshooting.
If older settings changed after a downgrade, back up and compare switch/RoomCircuit/data/ and config/RoomCircuit/data/ before choosing which data to keep.

Online updates
The title shows the installed version. The update button lights up when a new version is available. Press LS to check manually. Close the game before installing, then restart Atmosphère.
If installation is interrupted, open RoomCircuit Recovery from the Homebrew Menu.

Host or join
• Host on your own. Press Y in the lobby, choose a node, then confirm with A. Check Hosting line and press + to return to the game. New Local Wireless rooms use that node, including third-party nodes. In the node picker, press R to filter by region or X to test again.
• Your hosting node is remembered after reboot. In Ultrahand, open RoomCircuit and select Hosting node to change it. Leave the local-wireless session first.
• Recommendations. Up to three, based on this Switch's network. TCP/UDP identifies the probe, not in-game latency or other players' connections. Unverified regions are labeled.
• Join. Press Left to browse games, A to filter, and Right to return to rooms. ZL+ZR opens filters. A selects a room and checks your connection to its node. Continue or go back if it is weak, then search in the game.
• Test with friends. L in the node picker creates or joins a temporary code. Shared recommendations currently cover the 13 built-in nodes; custom nodes can be measured individually.
• Game route. Direct rooms use their public node. Legacy Community · LANywhere rooms may use Main server gateway; the connection check labels this. Recovery retries the same node. Rejoin if it fails.
• Passwords and alerts. Set the password in the game. Once your room appears as My room, you can share or hide its password in the lobby. Games check passwords on direct nodes. Appointment alerts use Main server.
• Compatibility. Standard and fork clients cannot mix in one room. The first accepted guest fixes the protocol until the host recreates the room. Use matching RoomCircuit and game versions. Node reachability does not prove every game works; see Nodes and compatibility and Validation.

Custom LAN Play nodes (Switch)
A LAN Play node is an internet-reachable service that relays Local Wireless game traffic between players. RoomCircuit's built-in nodes need no extra computer, server software or server hardware. To use an existing community node, get its address and port from its operator.
RoomCircuit's Main server is not a LAN Play node. Do not copy its address from config.ini into nodes.ini.

Add entries to config/RoomCircuit/nodes.ini on the SD card. Each [node] block is one endpoint; repeat the block for another node:
[node]
name=Amsterdam
host=eu-lanplay.example.net
port=11451
region=europe

[node]
name=Backup
host=backup.example.net
port=11451
region=unknown

[node] starts an entry. Repeat it for each endpoint.
name, host and port are required. name labels the entry, such as Amsterdam. host is the operator's public IPv4 address or hostname. port is the server's UDP listening port. 11451 is an example; use the port the operator provides.
region is an optional list filter label: unknown, asia, europe, americas, oceania or africa. It only categorizes the entry; it does not change routing or promise lower latency. If omitted, it is unknown.
These example hostnames are placeholders and cannot be used to connect. Up to seven local nodes are supported.

Save the file and reopen Nodes. Select an entry and press X to test it, then press A to choose it. For a node that is not in RoomCircuit's shared directory, both players must configure and select the same host and port, then search in the game's Local Wireless mode. Only an administrator-registered node can publish its rooms in the unified lobby.

You need a separate server only to operate a new node. The standard compatible server is [spacemeowx2/switch-lan-play](https://github.com/spacemeowx2/switch-lan-play#server). RoomCircuit implements its LAN Play protocol.
One-click setup on a Debian or Ubuntu VPS. Download tools/setup_lanplay_node.py, then run:
curl -fsSLO https://raw.githubusercontent.com/SEc-123/Roomcircuit_lanywhere-selfhost/main/tools/setup_lanplay_node.py
sudo python3 setup_lanplay_node.py
The script installs Docker if needed, starts the server, prompts for the public node address, name and region, and prints and saves a nodes.ini entry. Copy that entry to config/RoomCircuit/nodes.ini on each Switch. If UFW is active, it allows inbound UDP 11451. Open UDP 11451 in the VPS provider's cloud firewall too.
Run sudo python3 setup_lanplay_node.py --status on the VPS to see the connected-client count. The monitor returns a count only and is bound to localhost.
If Docker Compose is already installed, you can use https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/blob/main/deploy/lanplay-node/compose.yaml and run docker compose up -d in its folder. On the VPS, run curl http://127.0.0.1:11451/info to see connected clients. Choose the Python installer or Compose for this node.
RoomCircuit node settings have no username or password fields, so any device that can reach the UDP port can use the node. RoomCircuit includes the Switch client, so you do not need the upstream desktop lan-play client. A home server needs a public inbound route and router/firewall rules.

This file configures RoomCircuit on an Atmosphère Switch only. Android, desktop emulator and other mobile clients use their own network settings and need a compatible client build. See the Crossplay preview for the currently documented client-specific setup.

If both old and new nodes.ini files changed differently, back up both, keep the intended entries in config/RoomCircuit/nodes.ini, move the old file aside and reopen Nodes.

Appointments and pairing
• Press − or tap Appointments in the lobby to schedule a game. Press A to join or leave, L to create, Y for Ready, X to find rooms or B to return.
• Rooms on Main server can send appointment alerts. In-game alerts require Ultrahand with API notifications enabled.
• For pairing such as Pokémon Link Trade, both players choose local communication, use the same in-game code and start searching.

游戏候选示例 / Candidate games
Dying Light: Platinum Edition · Mario Kart 8 Deluxe · Monster Hunter Rise · Monster Hunter Generations Ultimate
Animal Crossing: New Horizons · Super Smash Bros. Ultimate · Stardew Valley · Luigi's Mansion 3
Overcooked! 2 · Diablo III: Eternal Collection · Mario Party Superstars · Kirby Fighters 2
Pokémon Scarlet · Pokémon Violet · Borderlands 2: Game of the Year Edition · Dragon Ball FighterZ
完整候选清单见 GAMES.txt。双机实测状态见项目的验证记录。
See GAMES.txt for the full candidate list and the project's validation record for two-console test status.
