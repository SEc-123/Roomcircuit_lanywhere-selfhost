# Online updates

Open LANywhere and select **Update** when a new version appears. Close the game before installing. Keep the app open until installation completes, then restart Atmosphère. Update files include the app, background module, overlay and bundled help. User settings and appointments are outside the managed file list.

For a first or manual install, extract the SD ZIP on a computer and merge `atmosphere/` and `switch/` into the card root. The update screen is available even when the background module is missing or incompatible. If installation is interrupted, open **LANywhere Recovery** from the Homebrew Menu and follow its status before restarting.

If **Update** asks you to move a leftover recovery app, move only `switch/lanywhere-recovery.nro` off the SD card, then retry. Keep your settings and appointments in `lanywhere/`.

## Publishing

The client fetches signed metadata and a hash-addressed package from the configured central relay over its existing TCP port. Ed25519 verification uses `common/update_public_key.h`. The private signing key stays with the release operator. The server serves public update artifacts and holds no signing key.

1. Build and verify the version-paired SD ZIP with the pinned toolchain. Run the updater packaging, transport, signature and transaction tests with the full host checks.
2. Run `scripts/make-online-update.py --sd-zip ZIP --key PRIVATE_PEM --output-dir OUTPUT --notes-en TEXT --notes-zh TEXT`. The public key must match the client key. Notes fit one short line per language.
3. Place the generated hash-named `.lup` file in `/opt/switch-online/updates` with read permission for the service. Publish `current.manifest` with an atomic rename after verifying the file is complete. Configure `LANYWHERE_UPDATE_DIR=/opt/switch-online/updates` and restart the relay when first enabling this service.
4. Read the manifest and package back through the public relay, verify their signature/hash, and compare the archive contents to the release SD ZIP. Keep the previous package while existing downloads can still use it.

The preset managed build uses this update feed. Configurable builds (`RELAY_PROFILE_FIXED=0`) use their matching SD package so an official preset cannot overwrite a custom server build. The manifest and file-ID map are versioned. Update file IDs must preserve their meaning for installed updaters. Future format changes require a supported migration release. This update source changes LANywhere program files on SD. Actual filesystem behavior during power loss and console reboot requires Switch acceptance testing.

## 中文

打开 LANywhere，出现新版时点「更新」。安装前关闭游戏，完成后重启 Atmosphère。设置和约局会保留。

首次或手动安装时，在电脑解压 SD 包，把 `atmosphere/` 和 `switch/` 合并到卡根目录。后台缺失或版本不匹配时仍可打开更新入口。安装意外中断时，从自制软件菜单打开「LANywhere Recovery」处理，再按提示重启。

如果「更新」提示恢复应用有残留，只将 `switch/lanywhere-recovery.nro` 移出 SD 卡，再试一次。保留 `lanywhere/` 中的设置和约局。
