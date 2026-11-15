## English

Fixes an online launch crash in the Album app. From 0.7.0 onward, checking for updates could abort because the Switch build of the signature library tried to use Unix random devices. 0.8.2 uses the Switch's kernel-seeded generator and still verifies the update signature.

If LANywhere crashes on launch, merge the full 0.8.2 SD ZIP into the card root and restart Atmosphère. Once 0.8.2 runs, future releases can be installed through **Update**, followed by a restart.

Node tests can now use the network connection initialized by the updater.

## 中文

修复从相册联网打开 LANywhere 时可能崩溃的问题。0.7.0 起，检查更新时验签库会尝试访问 Switch 上不存在的 Unix 随机设备。0.8.2 改用 Switch 自带的随机数接口，保留更新签名校验。

如果旧版一打开就崩溃，请将 0.8.2 完整 SD 包合并到卡根目录并重启虚拟大气层。之后可在 LANywhere 内点「更新」，安装后重启。

同时修复更新器先初始化网络后，节点测速无法启动的问题。
