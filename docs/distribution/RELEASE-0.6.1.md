## English

- Community connections now check that game UDP still receives replies. A broken data path triggers reconnection to the same room, even while the control connection stays online.
- The public room directory has a separate bounded connection slot. A full legacy gateway no longer hides the community hosting option.

Merge the full SD package into the SD root and restart Atmosphère. The hosting and joining controls are unchanged. Cross-network play on two Switch consoles and the latency reported in Issue #2 still need player confirmation.

## 中文

- 社区线路会持续检查游戏数据是否收到回应。即使控制连接正常，游戏数据断线也会尝试重连原房间。
- 为社区目录保留独立查询连接。旧网关连接占满时，社区建房选项仍可正常显示。

将完整 SD 包合并到卡根目录，重启 Atmosphère。建房和加入操作沿用原有教程。双 Switch 跨网对局及 Issue #2 的地区延迟仍待玩家实测。
