# RoomCircuit inside an emulator

The native RoomCircuit screen uses the relay's shared appointment store. The Switch app, the native screens and the optional web panel can create and read the same appointments and ready counts.

| Emulator | Native entry | Source and build notes |
| --- | --- | --- |
| Eden desktop | Multiplayer → RoomCircuit appointments | [Eden](../integrations/eden/README.md) |
| Eden Android | Settings → RoomCircuit | [Eden](../integrations/eden/README.md) |
| Ryubing and Kenji-NX desktop | RoomCircuit menu | [Ryubing and Kenji-NX](../integrations/ryubing/README.md) |
| Kenji-NX Android | Settings → System → RoomCircuit | [Ryubing and Kenji-NX](../integrations/ryubing/README.md) |
| MeloNX | Settings → Misc → Network → RoomCircuit | [Apple integrations](../integrations/apple/README.md) |
| NX Pocket | iOS: Settings or Local play. Mac: View → RoomCircuit | [Apple integrations](../integrations/apple/README.md) |

Set the native screen's server address to the operator's appointment service origin. For a LAN preview, start `deploy/start-crossplay-test.sh` as described in [Crossplay](CROSSPLAY.md). It prints the address on TCP 11464. On an installation that requires HTTPS, use the operator's [HTTPS endpoint](WEB-APPOINTMENTS.md). The UI connection settings and the game's own local wireless transport have separate ports.

Once an appointment is open, use the in-app room list to check the host. Follow the displayed transport settings to connect the emulator to the same relay, then use the game's native **Local Wireless** room browser. A ready action counts for five minutes. It does not reserve a game seat. The native screen works without opening a website.

## Native client contract

- `GET /api/appointments/options` returns `nativeApi: 1`, the verified game picker, enabled transports, `readyBefore` and `readyLease` in seconds. Each transport has `id`, `host`, `port` and `transport`.
- `POST /api/appointments/native` takes a JSON object. Send `Content-Type: application/json` and `X-RoomCircuit-Client: native-v1`. The native endpoint accepts app requests without browser-origin metadata. The browser endpoint retains its same-origin checks.
- Include `action` and a persistent, device-local 32-digit random hex `identity`. Never put that identity in a URL. The identity is for an anonymous ready lease, not an account or an authentication credential.
- `list` takes an optional integer `offset`. `event`, `rooms`, `ready` and `leave` take the appointment `id` as a decimal string. `create` takes `starts` as Unix seconds, `game` as a 16-digit local communication ID (LCID), and optional 16-digit `app` as a separate application ID. To avoid precision loss, keep IDs as strings.
- List, event and mutation responses include the relay's `now`, `rows` and next offset or `null`. Each row has `id`, `starts`, `game`, `app`, `interested` and `ready`. A successful create includes the exact `created` row. Room results contain the public host, seat counts, appointment binding, crossplay availability and whether the title matched the appointment.
- Use HTTPS on public networks, normal certificate validation, bounded requests and cancellation. Keep browser-only and device-local state, such as bookmarks, separate from the shared ready count.

## Verification

Run `node --test server/relay/test-appointments-web.js` from the source checkout. The test launches the public service, confirms that a native client and a browser can read an appointment created by either interface, exercises the real Switch protocol, and verifies public rooms from an emulator host. See each integration's build notes for the upstream source and platform checks.

## Upstream status

- [NX Pocket draft PR #1](https://github.com/zkkzkk/nx-pocket/pull/1) contains the optional native panel on iOS and macOS. The patched Mac application builds for arm64 and passes signature verification. Its launch on Apple Silicon and the complete iOS bundle still need validation. The draft remains open for maintainer review.
- The Eden contribution workflow requires human-authored PR text and disclosure for AI-assisted code. The source patches are available for a contributor's review.
- Ryubing, Kenji-NX and MeloNX source patches target their verified upstream revisions. Their source hosts need an authenticated contributor account for submission. Code and build notes are included here.
