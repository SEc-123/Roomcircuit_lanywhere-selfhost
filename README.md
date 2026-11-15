# LANywhere Self-host

[中文](#中文) · [Download](https://github.com/SEc-123/lanywhere-selfhost/releases/latest)

Play compatible Switch games' Local Wireless mode over the internet. Choose a public node using your Switch's connection test, or schedule a time and get room alerts on Main server.

1. Extract the SD ZIP on your computer. Merge `atmosphere/` and `switch/` into the SD card root, restart Atmosphère, and open **LANywhere Self-host** from Album. This replaces an existing ldn_mitm with the same module ID.
2. **Host on your own.** Press **Y** in the room lobby to choose a node. Use **R** to filter by region or **X** to test again. Choose a node, return to the game and create a room in **Local Wireless**.
3. **Join.** Find a room in the unified lobby; **ZL+ZR** filters game and region. Press **A**, check your connection to its node, then open the game and search in **Local Wireless**. A weak connection is shown before you decide to continue.

Up to three recommendations are **based on your network**, not future guests' connections. TCP/UDP labels identify the probe; these numbers are not in-game latency. Regions come from the node directory. Unverified regions stay unverified. **L** in the node picker offers an optional shared test code for friends; the host still selects a node and hosts normally.

Direct rooms use their actual public node; gameplay does not pass through Main server. Some legacy rooms use compatibility forwarding instead: their connection check explicitly measures **Main server gateway**, excluding the onward hop. A room keeps its node during recovery. If recovery fails, rejoin; it never silently moves the game to another node. [Nodes and compatibility](docs/REGIONS.md)

Standard and fork client protocols cannot be mixed in one room: the first accepted guest fixes the protocol until the host recreates the room. Using the same LANywhere version is the simplest setup.

**Main server** supports appointment alerts and lobby password sharing. Public nodes use the game's password. Press **←** or tap **Appointments** in the lobby to schedule a game. Press **B** or tap **All rooms** to return. In-game alerts require Ultrahand with API notifications enabled. **RS** switches English/Chinese.

Use **Update** to install future releases, then restart Atmosphère. Install the first updater-enabled version with the SD ZIP. [Online updates](docs/distribution/ONLINE-UPDATES.md)

Games that need a player name use the Switch name in System Settings automatically.

[Game candidates](compatibility/PLAYER-GAMES.txt) · [Validation status](docs/distribution/VALIDATION.md) · [Pack and store integration](docs/distribution/INTEGRATORS.md)

## Build and self-host

- Run `bash scripts/build-container.sh --embedded` to build the Switch app, sysmodule and overlay with the [pinned toolchain](scripts/toolchain/README.md).
- Run `npm start` with Node.js 22 or later. Forward TCP and UDP 11460. [Deployment guide](docs/DEPLOY.zh-CN.md).
- To target your own relay, edit [the connection profile](common/relay_profile.h) before building. [Optional managed regional relays](docs/distribution/REGIONAL-RELAYS.md)

## Credits

Based on [LANywhere by augustocco](https://github.com/augustocco/lanywhere) and [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm). [Third-party notices](third_party/README.md) · [GPL-2.0-only](LICENSE).

Development assisted by OpenAI Codex. Portions of the code were AI-generated.

## 中文

通过兼容 Switch 游戏的「本地无线」跨网联机。可由 Switch 实测后选择公共节点，也可提前约时间，在主服务器收开房提醒。

1. 在电脑解压[最新版](https://github.com/SEc-123/lanywhere-selfhost/releases/latest)的 SD 包，把 `atmosphere/` 和 `switch/` 合并到卡根目录。重启 Atmosphère 后从相册打开 **LANywhere Self-host**。安装会替换同模块 ID 的 ldn_mitm。
2. **单人建房。** 在房间大厅按 **Y** 选节点。**R** 筛选地区，**X** 重新测速。选好后回游戏，在「本地无线」里建房。
3. **加入。** 统一大厅查看各节点房间，**ZL+ZR** 筛选游戏和地区。按 **A** 选房，检查你到该节点的连接，再打开游戏，在「本地无线」里搜索。连接较差时先提示，由你决定是否继续。

最多三个推荐节点**根据你的网络**产生，不代表后来加入者的网络。TCP/UDP 表示测速方式，数值不是游戏内延迟。地区采用目录标注，未核实的不猜测。节点页按 **L** 可选「一起测速」，与好友共享临时码；建房者选定节点后照常建房。

直连房间的对局走房间实际节点，不经过主服务器。部分旧房间保留兼容转发；检查页会明确显示测的是「主服务器中转」，不包含后续转发耗时。断线恢复仍用原节点，不会自动换服；恢复失败需重新加入。[节点与兼容范围](docs/REGIONS.md)

同一房间不能混用标准与 fork 协议；首个被游戏接受的访客会确定协议，重建房间才能更换。建议大家使用同版 LANywhere。

约局提醒和大厅密码分享用「主服务器」，公共节点的密码在游戏里输入。在大厅按 **←** 或点「约局」可预约，按 **B** 或点「所有房间」返回。游戏内提醒需要 Ultrahand 开启 API 通知。按 **RS** 切换中英文。

安装首个带更新器的 SD 包后，今后可在应用内点「更新」，完成后重启虚拟系统。

游戏需要玩家名称时，自动使用系统设置中的 Switch 主机名称。

[游戏候选清单](compatibility/PLAYER-GAMES.txt) · [验证记录](docs/distribution/VALIDATION.md)
