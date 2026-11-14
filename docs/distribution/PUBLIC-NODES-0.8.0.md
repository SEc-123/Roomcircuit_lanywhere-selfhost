# Public node protocol checks · 0.8.0

Tested on 2026-11-14 UTC from the existing China service host, using the production native transport and independent standard/fork peers. Each test used a random synthetic game ID; no player room was joined. Both peers were on the same Linux host, communicating through the public relay. This is protocol evidence, not physical Switch or cross-region gameplay acceptance.

| Node | Standard host/join | Fork host/join | Shipping status |
| --- | --- | --- | --- |
| lanplay-nl | Pass / Pass | Pass / Pass | Enabled |
| lanplay-br | Pass / Pass | Pass / Pass | Enabled |
| lanplay-nl-jaysea-rust | Pass / Pass | Pass / Pass | Enabled |
| lanplay-nl-r3ps4j-rust | Pass / Pass | Pass / Pass | Enabled |
| lanplay-nl-r3ps4j-11452 | Pass / Pass | Pass / Pass | Enabled |
| lanplay-id | Pass / Pass | Pass / Pass | Enabled |
| lanplay-de-servegame | Failed / Failed | Failed / Failed | Disabled |
| lanplay-cn-jax | Pass / Pass | Pass / Pass | Enabled |
| lanplay-cn-gugu | Pass / Pass | Pass / Pass | Enabled |
| lanplay-cn-olunira | Pass / Pass | Pass / Pass | Enabled |
| lanplay-cn-shanghai | Pass / Pass | Pass / Pass | Enabled |

The ten passing nodes completed all 40 cases. Standard cases exchanged 1,472 / 4,096 / 65,507-byte UDP datagrams and 16 KB TCP streams in both directions. Fork cases exchanged 64 / 1,400 / 1,472-byte UDP and its legacy streams. Servegame replied to probes but failed room discovery/admission, so it is not offered for hosting.

Burst fragment loss was reproduced on Rust relays. A bounded native queue now paces fragmented UDP; the original failing public paths passed after the change. A separate capacity-ten relay regression also passes. The published [Rust relay queue](https://github.com/spacemeowx2/slp-server-rust/blob/master/src/slp/peer.rs) is consistent with the observed limit; the exact deployed server revisions were not identified.

## 中文

现有中国服务机用实际原生传输代码及独立协议客户端，经公网节点完成测试；双方测试进程在同一台 Linux 主机，不是两台 Switch，也不是跨地区玩家。每轮使用随机测试游戏身份，没有加入其他玩家房间。

10 个节点的标准/fork 建房、加入共 40 项通过，覆盖双向游戏 UDP 和流传输。Servegame 仅测速有回应，发现和准入失败，已停用建房入口。Rust 节点曾出现大包分片丢失；限速队列修复后，原失败路径复测通过。实体 Switch 游玩、跨网恢复和实际游戏延迟比较仍待验证。
