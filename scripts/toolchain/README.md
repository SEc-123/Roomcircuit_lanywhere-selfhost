# Switch 交叉构建与校验

复核当前验收包时，先在仓库根目录执行 `bash scripts/build-container.sh --embedded`。容器采用 `container-profile.json` 指定的 GCC 15.2.0 和固定 libnx 提交，并检查实际工具链版本。

以下原生 Linux 安装路径使用 `packages.lock.json` 中的 GCC 16.1.0 工具链，适用于单独构建。它需要 Python 3.11、Git、curl、GNU Make、tar 和 zstd，安装到 `sysroot/` 并核对软件包摘要。该路径的产物应使用自己的构建报告核验。

```bash
python3 scripts/toolchain/prepare.py
bash scripts/toolchain/build-switch.sh
```

已有匹配的 SDK 时，可以将 `DEVKITPRO` 指向其绝对路径。`DEVKITA64` 可单独覆盖，默认使用 `$DEVKITPRO/devkitA64`。macOS 的 SDK 安装和构建步骤见 [部署文档](../../docs/DEPLOY.zh-CN.md)。

```bash
export DEVKITPRO=/absolute/path/to/devkitpro
bash scripts/toolchain/build-switch.sh
python3 scripts/toolchain/verify_switch_build.py . \
  --app-title 'RoomCircuit' \
  --app-version "$(node -p 'require("./package.json").version')" \
  --ipc-abi 5 \
  --report evidence/switch-build.json \
  --build-command 'bash scripts/toolchain/build-switch.sh'
```

用实际执行的构建命令填写 `--build-command`。报告核对主应用、Tesla 浮层和后台模块的 AArch64 Build ID、名称、版本、ExeFS 内容和源文件哈希。`--ipc-abi 5` 还核对前后台声明的命令号、版本和数据结构大小。新旧后台与前台混装的界面行为要在真机上验证。

报告中的 `.bss` 是 ELF 静态零初始化区段大小。原后台约为 2.13 MiB，其中包括 1 MiB 全局分配区、128 KiB 服务分配区、320 KiB BSD 传输内存和 12 个各 16 KiB 的 IPC 线程栈。能力查询的返回值占 4 字节，匹配状态占 88 字节。检查 `.bss` 的差异可以发现额外常驻缓冲区，真实内存和会话压力要通过多台主机的完整对局验证。

在线更新器使用容器内的 libsodium 1.0.20 验证 Ed25519 签名。主机测试需要 `libsodium-dev` 和 zlib 开发库。
