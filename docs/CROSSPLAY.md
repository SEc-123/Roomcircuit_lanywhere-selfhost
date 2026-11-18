# RoomCircuit crossplay preview

Use the same Switch game and update on every device. Choose **Local Wireless** in the game. Test both directions by hosting once on Switch and once on the other device. Start with an open room on the test relay.

| Client | Connection |
| --- | --- |
| Atmosphère Switch | Install the configurable Switch test ZIP. Open RoomCircuit and set the test server IPv4 and port **11470**. Choose **Main server**. |
| Eden on Android or desktop | In Multiplayer, use **Direct Connect**, enter the test server IPv4 and UDP port **24872**, and join. Then search or host inside the game. This preview speaks Eden v0.2.1 and the compatible version-1 room protocol. Game data uses UDP. |
| Ryubing / Kenji-NX desktop | Set `ldn_server` in the emulator's JSON configuration to the test IPv4. Choose **RyuLDN** and disable P2P hosting. The tested protocol uses TCP **30456**. See the [version-specific settings](../server/ryuldn/README.md). |
| Kenji-NX Android | Use the included opt-in source patch for revision `280c427c550c5fe3bcc46b378e06a9b90a7e6b09b`. Set **Local wireless server** in System. Release Android builds that hardcode Disabled need this code rebuilt into the APK. |
| Ryujinx-family build with **LAN Play** | Set the test server IPv4 and UDP port **11451**. Use a build that explicitly offers LAN Play. |
| NX Pocket for iPhone or Mac | Use a supported game's **Local play** mode. Use the routed LAN Play setup below and set **Other devices' addresses** to `10.13.254.254`. |
| MeloNX for iOS / other builds with only **ldn_mitm** | Use the routed LAN Play setup below and select the phone's test Wi-Fi interface if the build offers a choice. This path needs device verification. |

### Start the preview

Run this on the computer hosting the relay. Replace the sample address with the computer's active LAN IPv4. Clients on other networks need to be able to reach these ports through a route or a firewall rule.

```sh
bash deploy/start-crossplay-test.sh 192.168.0.102
```

For a first test, connect the devices to the same Wi-Fi network. Install the test ZIP on a separate SD card or save your existing RoomCircuit configuration and files before swapping the builds. The test build accepts a different server in `config/RoomCircuit/config.ini`; use `server=192.168.0.102`, `port=11470` and `network=self`, then restart Atmosphère.

To reproduce the test ZIP from source, use a separate checkout. Set the default `RELAY_PROFILE_FIXED` to `0` in `common/relay_profile.h`, then run `bash scripts/build-container.sh --unconfigured --output /src/RoomCircuit-Crossplay-Switch-test.zip`. This builds all Switch components against the pinned toolchain.

### Apple clients using local Wi-Fi

Use a Linux gateway on the device's test Wi-Fi. This guide uses one client, with its own address and relay connection. The gateway must also have a working uplink. Example Wi-Fi address plan, on a dedicated test network:

- Linux Wi-Fi interface `wlan0`, address `10.13.250.1/30`.
- Apple device Wi-Fi address `10.13.250.2`, mask `255.255.255.252`, router `10.13.250.1`. Do not reuse that address for a second device.
- Enable IPv4 forwarding on the test gateway. Permit forwarding only between the provisioned client and the game network over `rcgame0`. Preserve the existing uplink route.

```sh
sudo ip tuntap add dev rcgame0 mode tun
sudo ip link set rcgame0 mtu 1400 up
sudo ip route add 10.13.0.0/16 dev rcgame0
sudo python3 tools/lanplay-tun/lanplay_tun.py \
    --tun rcgame0 --client 10.13.250.2 --wifi wlan0 \
    --relay 192.168.0.102:11451
```

The connected Wi-Fi /30 route keeps replies to the device on Wi-Fi. The TUN route carries the rest of the game subnet. The helper converts discovery broadcasts from the relay into unicast for the device and accepts the selected device's local discovery broadcasts. The mobile path is a companion gateway, and is still pending a full match on Apple hardware. For NX Pocket, choose a game with Local play in its profile.

### Operator and maintainer details

- `server/relay/emulator-bridge.js` uses the native RoomCircuit room and participant state. Native, LAN Play, Eden and RyuLDN players can share the same game room on that relay. Crossplay uses shared, open Local Wireless rooms. Password-protected RoomCircuit admission is outside this preview.
- `ROOMCIRCUIT_EDEN_PORT`, `ROOMCIRCUIT_RYULDN_PORT` and `LANYWHERE_LANPLAY_PORT` are opt-in. `ROOMCIRCUIT_EMULATOR_BIND` and `LANYWHERE_LANPLAY_BIND_ADDRESS` choose listening IPv4 interfaces. Build the pinned ENet worker on the target architecture with `sh server/eden/build.sh` before enabling Eden. Eden requires Node.js with the Zstd API (22.15 or newer, Node 24 recommended).
- Crossplay depends on the game's local communication ID, local wireless version and sockets. RyuLDN supports the tested UDP and TCP stream paths. Eden's emulator socket proxy and this standard LAN Play bridge support UDP game traffic. TCP used for LAN Play discovery and join is separate. Compatibility per game needs a completed real match or in-game trade.
- `ROOMCIRCUIT_TEST_ENET=1 npm test` includes the ENet transport and crossplay tests. Linux TUN validation uses `docker run --rm --cap-add NET_ADMIN --device /dev/net/tun -v "$PWD/tools/lanplay-tun:/test:ro" python:3.12-alpine python /test/test_linux.py`.

### 中文试用

1. 在电脑上运行 `bash deploy/start-crossplay-test.sh 电脑的局域网IPv4`。首次测试用同一个 Wi-Fi。
2. Switch 装可配置测试包，RoomCircuit 填电脑 IP 和 **11470**，重启大气层并选 **主服务器**。
3. 安卓 Eden 进 **多人 → 直连**，填同一个 IP 和 **24872**。龙神选支持自定义地址的 **RyuLDN**，用 **30456**。支持 **LAN Play** 的版本，用 **11451**。Kenji 安卓需用随源码提供的补丁构建，iPhone 本地联机模式需通过上面的 Linux 网关。
4. 打开同一款、同版本游戏，进「本地通信」。两端轮流建房，各玩完一局。宝可梦本地交换以双方实际完成一次交换为准。
