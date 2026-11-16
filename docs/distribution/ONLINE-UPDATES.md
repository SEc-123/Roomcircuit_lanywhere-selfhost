# Online updates

Open RoomCircuit and select **Update** when a new version appears. Close the game before installing. Keep the app open until installation completes, then restart Atmosphère. Follow any further restart prompt and open **RoomCircuit** from the new `switch/RoomCircuit/` location. The update includes the app, background module, overlay and bundled help; settings, custom nodes and appointments are not replaced.

For a first or manual install, extract the SD ZIP on a computer and merge `atmosphere/`, `switch/` and `config/` if present into the card root. The update screen is available even when the background module is missing or incompatible. If installation is interrupted, open **RoomCircuit Recovery** from the Homebrew Menu and follow its status before restarting.

On restart, recognized settings and appointments are copied from the previous `switch/RoomCircuit/data/` or `lanywhere/` to `config/RoomCircuit/data/`; the originals are retained. Pending recovery or conflicting data can defer this migration. Keep both copies until recovery is complete. Custom node definitions are stored in `config/RoomCircuit/nodes.ini`.

Older versions do not read the new data folder. If an older version changes its retained settings after a downgrade, back up and compare both data folders before deciding which changes to keep; they are not merged automatically.

If **Update** asks you to move a conflicting file, move the exact path shown off the SD card and retry. For a leftover recovery app, that path is `switch/lanywhere-recovery.nro`. The updater keeps `lanywhere/.update/` and this recovery path for compatibility; do not remove them while an update is pending.

**Uninstall can delete settings and appointments.** Back up `config/RoomCircuit/` and any older settings folders before choosing a full removal.

## Publishing

The client fetches signed metadata and a hash-addressed package from the configured central relay over its existing TCP port. Ed25519 verification uses `common/update_public_key.h`. The private signing key stays with the release operator. The server serves public update artifacts and holds no signing key.

1. Build and verify the version-paired SD ZIP with the pinned toolchain. Run the updater packaging, transport, signature and transaction tests with the full host checks.
2. Run `scripts/sign-entrypoints.py --sd-zip UNSIGNED_ZIP --output SD_ZIP --key PRIVATE_PEM`. Distribute the signed ZIP; its entrypoint proof lets future updates identify the installed files safely. Keep the proof in integration ZIPs.
3. Run `scripts/make-online-update.py --sd-zip SD_ZIP --key PRIVATE_PEM --output-dir OUTPUT --notes-en TEXT --notes-zh TEXT`. The public key must match the client key. Notes fit one short line per language.
4. Place the generated hash-named `.lup` file in `/opt/switch-online/updates` with read permission for the service. Publish `current.manifest` with an atomic rename after verifying the file is complete. Configure `LANYWHERE_UPDATE_DIR=/opt/switch-online/updates` and restart the relay when first enabling this service.
5. Read the manifest and package back through the public relay, verify their signature/hash, and compare the archive contents to the release SD ZIP. Keep the previous package while existing downloads can still use it.

The preset managed build uses this update feed and the official signing steps above. Configurable builds (`RELAY_PROFILE_FIXED=0`) use their matching SD package; their extra server bootstrap is not part of the official signed updater. The manifest and file-ID map are versioned. Update file IDs must preserve their meaning for installed updaters; the package builder maps the renamed SD paths to the existing IDs. Future format changes require a supported migration release. Actual filesystem behavior during power loss and console reboot requires Switch acceptance testing.

## 中文

打开 RoomCircuit，出现新版时点「更新」。安装前关闭游戏，完成后重启 Atmosphère；如仍有重启提示，按提示操作，然后从新的 `switch/RoomCircuit/` 入口打开。更新不会覆盖设置、自定义节点和约局。

首次或手动安装时，在电脑解压 SD 包，把 `atmosphere/`、`switch/` 和存在的 `config/` 合并到卡根目录。后台缺失或版本不匹配时仍可打开更新入口。安装意外中断时，从自制软件菜单打开「RoomCircuit Recovery」处理，再按提示重启。

重启后，旧的设置和约局会从 `switch/RoomCircuit/data/` 或 `lanywhere/` 校验复制到 `config/RoomCircuit/data/`，原文件保留。更新恢复未完成或数据冲突时迁移可能延后；恢复完成前请保留两处文件。自定义节点保存在 `config/RoomCircuit/nodes.ini`。

旧版无法读取新数据目录。降级后若旧版修改过设置，请备份并对比两个数据目录，再决定保留哪些变更；它们不会自动合并。

如果提示文件冲突，按提示将对应文件移出 SD 卡再重试。恢复应用残留的路径是 `switch/lanywhere-recovery.nro`。为兼容旧版本，更新事务仍使用 `lanywhere/.update/` 和这个恢复应用路径；更新未完成时不要删除。

**卸载可能删除设置和约局。** 完全移除前，请备份 `config/RoomCircuit/` 和旧版设置目录。
