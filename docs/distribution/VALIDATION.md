# Validation

- 0.4.6. One physical Switch confirmed lobby names and icons for **Dying Light** and **Pokémon Violet** after hosting Local Wireless rooms on the preset relay.
- 0.4.7. See the [release](https://github.com/SEc-123/lanywhere-selfhost/releases/tag/v0.4.7) and [automated checks](https://github.com/SEc-123/lanywhere-selfhost/actions).
- 0.5.4 Beta. Host regression tests and Switch cross-build passed. Physical Switch cross-network joining remains unverified.
- 0.5.7 Beta. All 97 host test suites and the Switch cross-build passed. A physical Switch booted into emuMMC with an existing appointment, handled appointment buttons, and rebooted successfully. The Pokémon-style search and join path passed two-peer relay tests. A physical two-console trade and the Ultrahand popup remain unverified.
- 0.5.8 candidate. All 97 host suites and the Switch cross-build passed. A host test of the production startup check reproduced 0.5.7 rejecting HOS 23.5.0 and verified the updated module accepts the simulated version. Physical boot, system service IPC and two-console play on 23.5.0 remain pending. [Firmware compatibility and test matrix](HOS-COMPATIBILITY.md)
- 0.6.0 candidate. With the pinned original LANywhere relay, two simulated clients found, joined and exchanged game packets while the directory process was stopped, then resumed on the same endpoint. From the current China server, the public relay passed a private-room create/join test, bidirectional UDP at 64, 1455 and 1472 bytes, and seat recovery. An isolated directory instance on that server correctly offered the public endpoint. The Mac test route received a TCP close without PONG. Public gameplay between two Switch consoles and regional latency remain unverified.
- 0.6.2 Beta. The extracted Switch UDP worker was reproduced failing to recover in 0.6.0, then passed real-loopback socket tests for UDP-only loss with healthy TCP, wrong-source replies, cancellation and restored game traffic. A separate concurrency regression verifies UDP sends complete before recovery closes the socket. A fixed original upstream also kept community listings available with all eight legacy gateway connections occupied. These host tests use an accelerated clock and a socket-install seam for recovery. Physical two-console play and the reported regional latency remain unverified.
- 0.7.0 Beta. The online updater verifies an Ed25519 manifest, installs a fixed managed-file set and retains a recovery entry. Host tests cover interrupted installs and rollbacks, invalid signatures and packages, download cancellation, startup/version checks and preserved settings. A production-key package was downloaded through the live relay and installed into an isolated host filesystem. Physical Switch installation, reboot confirmation and SD power-loss behavior remain unverified.

## 0.8.0 Beta — node selection

Verified during development:

- The Album app and sysmodule cross-built for Switch. A host cross-build is not a physical-console boot test.
- Actual TCP/UDP probe code passed six loopback cases: delayed TCP replies, both public UDP echo formats, unrelated packets, partial response loss and cancellation. Recommendation tests cover region scope, shared worst-player results, stale samples, network changes and resume.
- The current app input tests passed 205 cases. Additional full filter-loop tests cover preserving the selected game, search, clearing, canceling, held touch and the exact rendered node identity. Catalog tests cover raw-ID filtering; an unresolved sentinel is not assigned to a known game.
- Real loopback relay tests exercised standard/fork room locators, direct peer admission after the directory stopped, and temporary shared tests without creating a game room. Directory tests also cover source isolation, stable pages and participant expiry.
- Host rendering covered 82 lobby states, 16 node states and 16 guide pages in English/Chinese. The guide passed 296 text-boundary/glyph checks. These use host fonts and declared sample data, not Switch screenshots.
- A read-only check from the China service host on 2026-11-14 received valid UDP PING echoes from 11 configured Switch LAN Play endpoints across nine hostnames. It did not register players, join rooms or run games. This is not a measurement from a player's Switch.

- Public native host/join checks passed on ten LAN Play nodes: 40 standard/fork cases, both game-stream origins and UDP up to 65,507 bytes on standard transport. Servegame failed discovery/admission and is disabled. Both fixture peers ran on the same China Linux host, through each public relay. [Per-node results](PUBLIC-NODES-0.8.0.md)
- A capacity-ten relay regression reproduces the fragmented UDP pressure and verifies paced delivery. Separate checks cover first accepted protocol locking, exact selected sessions in crowded scans, socket rebind, and the Switch SDK thread-priority conversion.

Reproducible checks: [probe tests](../../app/tests/node_probe_test.py), [recommendations and recovery state](../../app/tests/nodes_model_test.py), [filter input](../../app/tests/nodes_input_test.py), [room identity parser](../../app/tests/node_list_test.py), [direct locators](../../server/relay/test-node-real-locator.js), [native transport](../../console/ldn_mitm/tests/lanplay_direct_test.cjs), and [node UI rendering](../../app/tests/preview/render_nodes.py).

Release artifacts: app, background module and overlay identify as 0.8.0; the source/build fingerprint and IPC ABI 5 checks pass. The SD ZIP contains 24 files, and the signed updater retains the existing 22 managed-file IDs. SD ZIP SHA-256: `0e61e15a7cf28573f87189f7d878edbc4f55f07945ac7e3a52c733cd18300733`.

The production directory returned the matching catalog fingerprint, 12 enabled/available entries (My server, original LANywhere and ten LAN Play nodes), and valid shared recommendations for two temporary participants. Both test participants were removed. Release publication requires the repository CI and signed-update readback; the [release assets and checks](https://github.com/SEc-123/lanywhere-selfhost/releases/tag/v0.8.0) identify the distributed build.

Still required before claiming hardware compatibility or latency improvement:

- Physical Switch install, matching foreground/background versions, boot and restart on the supported HOS/Atmosphère combinations.
- Two physical Switch consoles: solo public hosting; a stranger joining later; players in different regions joining the same exact room; optional friend measurement; standard/fork client compatibility and incompatible-client rejection.
- Actual game traffic, Wi-Fi change, sleep/wake, node outage and same-node recovery. A timeout may require rejoining; seamless game recovery is not established.
- With the same players, game and version, compare routes using measured gameplay delay and disconnects. Endpoint RTT, protocol fixtures and room visibility do not establish a gameplay improvement.

### 中文

0.8.0 开发验证已覆盖 Switch 交叉构建、真实回环 TCP/UDP 测速、推荐与缓存失效、房间身份、目录退出后的协议连接、205 项原有输入场景及新增筛选测试；另完成 114 张主机渲染图和 296 项教程文字检查。中国服务机对 11 个公共入口的只读 PING 有回应，但没有在这些入口上完成真实双机游戏。

Switch 实机安装与重启、双机建房加入、跨地区同房、好友测速、标准/fork 互通边界、换网和断线恢复，以及同组玩家的实际延迟对比均保留为待验收。不能据此宣称所有游戏可玩或延迟已经降低。


## 0.8.1 Beta — saved LAN Play address

Source inspection found that 0.8.0 accesses `sdmc:/lanywhere/lanplay-ip.bin` through C stdio, while this sysmodule mounts the SD card through Atmosphère's filesystem interface. The 0.8.1 change targets that persistence mismatch.

The actual Switch storage function passed host filesystem-boundary tests for saved identity reuse, missing/corrupt records, collision renewal, failed writes/renames and previous-file recovery. A clean Switch cross-build and paired 0.8.1 metadata/source verification passed. SD ZIP SHA-256: `27d956cd765b754d205ff1a6e337e5cc434883c0c16e1dbfa8b302de6a93a58a`. Release publication also requires CI and signed-update readback. Physical Switch restart/address reuse and two-console play remain unverified. The 0.8.0 public-node results above describe unchanged transport behavior, not a second hardware test.

中文：已定位 0.8.0 的 LAN Play 地址保存接口与模块实际 SD 挂载方式不一致；0.8.1 针对此问题修复。实际 Switch 存储函数的主机边界回归、干净交叉构建与同版组件校验通过；发版仍须 CI 和签名更新回读。实机重启后的地址复用和双机游戏仍待实测。

## 0.8.2 Beta — online launch from Album

Issue #3 reports an immediate online crash in 0.7.0 and a successful launch in 0.6.2. Version 0.6.2 had no automatic update check. In the 0.7.0 and 0.8.1 release binaries, a complete update manifest reaches Sodium initialization before version comparison. The linked random backend attempts to open Unix random devices that libnx does not provide. Failure reaches the library's abort path. An unreachable update server returns a network error before initialization.

0.8.2 registers libnx's kernel-seeded generator once before Sodium initialization. The updater still verifies the Ed25519 signature before trusting the manifest. The change applies to automatic checks, manual checks and installation verification. Issue #3 remains open until the reporter can retest on a Switch.

中文：0.6.2 启动时没有更新检查。0.7.0 和 0.8.1 收到清单后会初始化验签库，默认 Unix 随机设备无法打开时会中止进程。0.8.2 改用 libnx 提供的随机数源，继续校验签名。报告者的实机复测仍待完成。

The 0.7.0 and 0.8.1 parsers aborted on the genuine signed release manifest in a controlled build of the pinned Sodium version with Unix random facilities unavailable. The Switch code regression checks one-time entropy registration with concurrent checks and ensures that valid signatures pass and modified manifests fail. A separate test covers opening the node picker after the updater has registered libnx networking, then releasing that socket only from its owner.

The reporter's subsequent logs identify the release build of 0.8.1 on HOS 23.0.1 with Atmosphère 1.12.2. The updater thread's return addresses match `check_worker` → `update_manifest_parse` → `sodium_init` → `randombytes_stir` → `sodium_misuse` → `abort` in that exact binary. The same thread then entered libnx cleanup, which closes HID before exiting the applet. The main thread's `2345-0008` report follows from polling HID during that cleanup. The HOME Menu report arrived one second later. The reporter's retest remains the check for that final UI outcome.

## 0.8.3 Beta — combined startup, network and installation review

- Startup, manual check and installation share the signed manifest verification. The host regression checks native entropy registration, valid signatures and tamper rejection.
- Updater and node tests acquire the shared libnx network in either order. The regression covers failed initialization, retry, partial worker startup and releasing the socket after both features finish.
- A fresh manual install needs the five runtime files under `atmosphere/` and `switch/`. User data remains separate from the install and the signed update file map remains compatible.
- Interrupted update rollback and an unowned Recovery app are covered separately. The explicit conflict preserves the file and settings, and the signed update succeeds after moving the leftover file away. The reporter's physical console retest remains open.
