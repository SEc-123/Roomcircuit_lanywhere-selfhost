# Integrating LANywhere Self-host

Download the [latest integration ZIP](https://github.com/SEc-123/lanywhere-selfhost/releases/latest/download/switch-online-integration.zip). Copy `atmosphere/` and `switch/` to the SD root. Keep the help and license notices with the distributed archive.

| Path | Purpose |
| --- | --- |
| `switch/lanywhere.nro` | Album lobby |
| `switch/.overlays/lanywhere.ovl` | Tesla-compatible status and controls |
| `atmosphere/contents/4200000000000010/` | Required local wireless module and boot flag |

The module replaces ldn_mitm with the same program ID. Offer it as an alternative if the pack already includes a provider of that ID. Keep any existing overlay loader and menu; they are separate dependencies. Restart after installation.

An install or upgrade can leave existing `lanywhere/` settings in place. In Ultra Tuner, **Remove** clears the module's saved settings and appointments along with the runtime files.

Source is available under GPL-2.0-only; see the release for the exact project and linked-library sources.
