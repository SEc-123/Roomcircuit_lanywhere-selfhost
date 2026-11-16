# Integrating RoomCircuit

Play compatible Switch Local Wireless games online. Find rooms across nodes, test your connection, or schedule a session.

Download the [latest integration ZIP](https://github.com/SEc-123/lanywhere-selfhost/releases/latest/download/switch-online-integration.zip). Copy `atmosphere/` and `switch/` to the SD root. Keep the help and license notices with the distributed archive.

| Path | Purpose |
| --- | --- |
| `switch/RoomCircuit/RoomCircuit.nro` | Album lobby |
| `switch/RoomCircuit/nodes.ini` | Optional user-defined LAN Play nodes; not included in the ZIP |
| `switch/RoomCircuit/data/` | Migrated settings, appointments and selected node |
| `switch/RoomCircuit/.entrypoints-signed` | Signed ownership proof required for safe future updates |
| `switch/.overlays/RoomCircuit.ovl` | Tesla-compatible status and controls |
| `atmosphere/contents/4200000000000010/` | Required local wireless module and boot flag |

The module replaces ldn_mitm with the same program ID. Offer it as an alternative if the pack already includes a provider of that ID. Keep any existing overlay loader and menu; they are separate dependencies. Restart after installation.

On restart, the module copies recognized user data from `lanywhere/` into `switch/RoomCircuit/data/` after checking it. It keeps the old files; an unfinished update or conflicting data can defer migration. Preserve both directories and follow the on-screen recovery instructions. A verified new app and matching module can remove recognized old app and overlay binaries; unknown old files are kept.

Do not include a `nodes.ini` or user data in an integration ZIP. Merge `atmosphere/` and `switch/` instead of replacing whole directories. The legacy updater still uses `lanywhere/.update/` and `switch/lanywhere-recovery.nro` for recovery; do not remove them during an update.

**Uninstall can delete player data.** Ultra Tuner's existing Remove entry deletes the old `lanywhere/` settings and appointments. Its current rules do not yet remove `switch/RoomCircuit/` or the renamed overlay. A candidate update to remove both old and new files, including the new settings and appointments, awaits the maintainer. Back up user data before selecting a full removal.

Source is available under GPL-2.0-only; see the release for the exact project and linked-library sources.
