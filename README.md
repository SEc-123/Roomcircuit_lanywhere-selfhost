# RoomCircuit (formerly LANywhere Self-host)

[Download](https://github.com/SEc-123/lanywhere-selfhost/releases/latest) · [中文](#中文)

Play compatible Switch games online through their Local Wireless mode. Browse rooms by game, test network routes on your Switch, and schedule sessions with other players.

## Install

Merge `atmosphere/` and `switch/` from the SD ZIP into your SD card root. Restart Atmosphère, then open **RoomCircuit** from Album. This replaces any existing ldn_mitm with the same module ID. Existing RoomCircuit settings carry over.

## Play

- **Join.** Press **←** to browse games, **A** to filter, and **→** to return to rooms. Select a room, then search in the game's **Local Wireless** mode.
- **Host.** Press **Y** to choose a node. Press **R** to change region, **X** to measure again, or **L** to test with friends. Return to the game and create a Local Wireless room.
- **Schedule.** Press **−** for appointments on Main server. In-game room alerts require Ultrahand with API notifications enabled.
- **Settings.** Press **RS** to switch languages, **→** from the room list for the guide, and **LS** to check for updates. Restart Atmosphère after installing an update.

[Node routing and compatibility](docs/REGIONS.md) · [Online updates](docs/distribution/ONLINE-UPDATES.md) · [Pack and store integration](docs/distribution/INTEGRATORS.md)

## Build and self-host

- Build with `bash scripts/build-container.sh --embedded` using the [pinned toolchain](scripts/toolchain/README.md).
- Start the relay with `npm start` on Node.js 22 or later. See the [deployment guide](docs/DEPLOY.zh-CN.md) and [connection profile](common/relay_profile.h).

## 中文

用 Switch 游戏自带的「本地无线」和异地玩家联机。可按游戏找房、测速选线路，也可提前约局。

**安装**　将[最新 SD 安装包](https://github.com/SEc-123/lanywhere-selfhost/releases/latest)的 `atmosphere/` 和 `switch/` 合并到卡根目录。重启大气层，从相册打开 **RoomCircuit**。安装会替换同模块 ID 的 ldn_mitm，并保留 RoomCircuit 原有设置。

- **加入。** 按 **←** 浏览游戏，**A** 筛选，**→** 回房间。选房后在游戏的「本地无线」里搜索。
- **建房。** 按 **Y** 选节点，**R** 切地区，**X** 重测，**L** 和好友一起测速。选好后回游戏，在「本地无线」里建房。
- **约局。** 按 **−** 打开主服务器约局。游戏内开房提醒需要 Ultrahand 开启 API 通知。
- **设置。** 按 **RS** 切换语言，在房间列按 **→** 看教程，按 **LS** 检查更新。安装更新后重启大气层。

[节点与兼容范围](docs/REGIONS.md) · [在线更新](docs/distribution/ONLINE-UPDATES.md) · [服务端部署](docs/DEPLOY.zh-CN.md)

## Credits

Based on [LANywhere by augustocco](https://github.com/augustocco/lanywhere) and [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm). [Third-party notices](third_party/README.md) · [GPL-2.0-only](LICENSE).

Development assisted by OpenAI Codex. Portions of the code were AI-generated.
