# Integrating RoomCircuit

Play compatible Switch Local Wireless games online. Find rooms across nodes, test your connection, or schedule a session.

Download the [latest integration ZIP](https://github.com/SEc-123/lanywhere-selfhost/releases/latest/download/switch-online-integration.zip). Merge `atmosphere/`, `switch/` and `config/` if present into the SD root. Keep the help and license notices with the distributed archive.

| Path | Purpose |
| --- | --- |
| `switch/RoomCircuit/RoomCircuit.nro` | Album lobby |
| `config/RoomCircuit/nodes.ini` | Optional user-defined LAN Play nodes; not included in the ZIP |
| `config/RoomCircuit/data/` | Migrated settings, appointments and selected node |
| `switch/RoomCircuit/.entrypoints-signed` | Signed ownership proof required for safe future updates |
| `switch/.overlays/RoomCircuit.ovl` | Ultrahand-compatible status and node selection |
| `atmosphere/contents/4200000000000010/` | Required local wireless module and boot flag |

The module replaces ldn_mitm with the same program ID. Offer it as an alternative if the pack already includes a provider of that ID. Keep any existing overlay loader and menu; they are separate dependencies. Restart after installation.

On restart, the module migrates recognized settings from the previous `switch/RoomCircuit/` or `lanywhere/` data into `config/RoomCircuit/`. It keeps old files; an unfinished update or conflicting data can defer migration. Preserve both copies and follow the on-screen recovery instructions.

Do not include a `nodes.ini` or user data in an integration ZIP. Merge folders instead of replacing whole directories. The legacy updater still uses `lanywhere/.update/` and `switch/lanywhere-recovery.nro` for recovery; do not remove them during an update.

Ultra Tuner's maintainer manages its integration. Its current full removal also removes the background module; deleting only the NRO keeps the module. Its current installation script removes `lanywhere/`, so users upgrading from an older installation should back up that directory before using it. Do not use an installer while an online update is pending.

Source is available under GPL-2.0-only; see the release for the exact project and linked-library sources.
