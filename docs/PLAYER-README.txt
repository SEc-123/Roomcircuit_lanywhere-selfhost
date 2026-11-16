RoomCircuit

中文

让支持本地无线的 Switch 游戏跨网联机。跨节点找房、测速选线路，或提前约局。

安装
在电脑解压，把 atmosphere 和 switch 合并到 SD 卡根目录。覆盖同名文件，保留原目录和设置。说明与许可可在电脑查看。
安装会替换相同模块 ID 的 ldn_mitm。重启进入兼容的 Atmosphère，再从相册打开 RoomCircuit。
   @CONNECTION_ZH@
默认英文。点右上角或按 RS 切换中英文，设置会保留。右下角可「查看教程」。
游戏需要玩家名称时，自动使用 Switch 主机名称。
应用、浮层与数据分别位于 switch/RoomCircuit/RoomCircuit.nro、switch/.overlays/RoomCircuit.ovl 和 switch/RoomCircuit/。
自定义服务器安装包保留已有配置；新旧配置冲突时不会覆盖原文件。若仍有旧入口，请选择新路径中的 RoomCircuit.nro，确认后再手动移除旧入口。
若数据目录存在但后台无法加载，请先备份 SD 卡的 switch/RoomCircuit/data/。确认这是原有数据且其中缺少隐藏文件 .storage-v1 后，在该目录创建同名空文件，再重启虚拟系统；不确定时保留原目录并寻求帮助。

在线更新
标题旁显示当前版本。有新版时，顶部高亮提示。按 LS 可手动检查更新。关闭游戏后安装，完成后重启虚拟系统。
安装中断时，从自制软件菜单打开 RoomCircuit Recovery。

建房与加入
• 单人建房。大厅按 Y 选节点，按 A 确认。看一眼底部「建房线路」，再按 + 回游戏，在「本地无线」里建房。新房间会用这条线路，选了第三方节点就用该节点建房。节点页按 R 筛地区，X 重新测速。
• 重启后保留所选建房节点。浮层分别显示「建房节点」和当前「房间节点」；节点暂时不可用时会显示状态。
• 推荐。最多三个，根据你这台 Switch 的网络产生。TCP/UDP 是测速方式，不是游戏内延迟，也不代表其他玩家的网络。地区未核实会明确标注。
• 加入。左键浏览游戏，A 筛选，右键回房间。ZL+ZR 可筛游戏和地区。A 选房，查看你到房间节点的连接，再打开游戏搜索。连接较差时可选择继续或返回。
• 一起测速。节点页按 L 创建或输入临时码，共同推荐当前覆盖 13 个内置节点。自定义节点可单独测速。
• 对局线路。直连房走房间实际公共节点；旧版「社区 · LANywhere」等房间可能经主服务器兼容转发，连接检查会明确标注。断线只尝试原节点，恢复失败需重新加入。
• 密码与提醒。在游戏内设密码。自己的房间显示「我的房间」后，可在大厅分享或隐藏游戏密码。第三方直连时仍在游戏里验密码。约局提醒使用主服务器。
• 兼容范围。标准和 fork 协议不能混房；首个被游戏接受的访客确定协议，重建房间可更换。建议同版 RoomCircuit、同版游戏。节点可达不代表每款游戏已实测，详见项目的节点说明与验证记录。

自定义节点
在 switch/RoomCircuit/nodes.ini 中按下面格式添加节点，保存后重新打开节点页：
[node]
name=Russia
host=lanplay.example.org
port=11451
region=europe
name、host、port 为必填。地址为示例，请替换为服务器运营者提供的公网域名或 IPv4 和 UDP 端口。最多 7 个本机节点；region 可省略，或填写 unknown、asia、europe、americas、oceania、africa。
管理员收录的节点及房间会出现在统一大厅。本机未收录节点需双方都配置相同地址并选择，再到游戏内搜索。

约局与双人配对
• 在大厅按 − 或点「约局」安排游戏。约局页按 A 参加或退出、L 发起、Y 准备、X 找房，按 B 回大厅。
• 用「主服务器」开房可发出约局提醒。游戏内弹窗需要 Ultrahand 启用 API 通知。
• 宝可梦连接交换等双人配对，两边在游戏里选本地通信，输入相同连接码并开始搜索。

English

Play compatible Switch Local Wireless games online. Find rooms across nodes, test your connection, or schedule a session.

Install
Extract on a computer and merge atmosphere and switch into the SD card root. Read the guide and license notices on the computer.
Overwrite matching files, but do not delete or replace the whole existing atmosphere folder.
This replaces an existing ldn_mitm with the same module ID. Restart into compatible Atmosphère and open RoomCircuit from Album.
   @CONNECTION_EN@
Use the top-right language control or press the right stick (RS) to switch languages. Your selection is remembered.
Choose View guide at the bottom right for help.
Games that need a player name use your Switch name automatically.
The app, overlay and data are in switch/RoomCircuit/RoomCircuit.nro, switch/.overlays/RoomCircuit.ovl and switch/RoomCircuit/.
Custom-server packages preserve existing settings and do not overwrite conflicting configurations. If an old entry remains, use RoomCircuit.nro at the new path and check the old entry before removing it.
If the data folder exists but the module cannot load, first back up switch/RoomCircuit/data/. If it is your existing data and the hidden .storage-v1 file is missing, create an empty file with that name in the folder and restart Atmosphère. If unsure, keep the folder and seek help.

Online updates
The title shows the installed version. The update button lights up when a new version is available. Press LS to check manually. Close the game before installing, then restart Atmosphère.
If installation is interrupted, open RoomCircuit Recovery from the Homebrew Menu.

Host or join
• Host on your own. Press Y in the lobby, choose a node, then confirm with A. Check Hosting line and press + to return to the game. New Local Wireless rooms use that node, including third-party nodes. In the node picker, press R to filter by region or X to test again.
• Your hosting node is remembered after reboot. The overlay shows both Hosting node and the active Room node, including when a node is unavailable.
• Recommendations. Up to three, based on this Switch's network. TCP/UDP identifies the probe, not in-game latency or other players' connections. Unverified regions are labeled.
• Join. Press Left to browse games, A to filter, and Right to return to rooms. ZL+ZR opens filters. A selects a room and checks your connection to its node. Continue or go back if it is weak, then search in the game.
• Test with friends. L in the node picker creates or joins a temporary code. Shared recommendations currently cover the 13 built-in nodes; custom nodes can be measured individually.
• Game route. Direct rooms use their public node. Legacy Community · LANywhere rooms may use Main server gateway; the connection check labels this. Recovery retries the same node. Rejoin if it fails.
• Passwords and alerts. Set the password in the game. Once your room appears as My room, you can share or hide its password in the lobby. Games check passwords on direct nodes. Appointment alerts use Main server.
• Compatibility. Standard and fork clients cannot mix in one room. The first accepted guest fixes the protocol until the host recreates the room. Use matching RoomCircuit and game versions. Node reachability does not prove every game works; see Nodes and compatibility and Validation.

Custom nodes
Add entries to switch/RoomCircuit/nodes.ini. Reopen Nodes to load changes:
[node]
name=Russia
host=lanplay.example.org
port=11451
region=europe
`name`, `host` and `port` are required. Replace the example with the operator’s public hostname or IPv4 and UDP port. Up to seven local nodes are supported. Region is optional: unknown, asia, europe, americas, oceania or africa.
Administrator-published nodes and rooms appear in the shared lobby. For unregistered local nodes, both players configure and select the same endpoint, then search in the game.

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
