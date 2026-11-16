# RoomCircuit identity lab

Run the OpenPak account core, PostgreSQL and the RoomCircuit fixture identity service on an isolated Docker network. Only the account and identity HTTP ports are published, both on `127.0.0.1`. The account core's development mail flow returns the email verification token directly for synthetic test accounts.

```sh
sh build.sh
sh lab.sh up
python3 -B smoke.py
```

The build checks out OpenPak account revision `96b0593eea532d10d87b284146c4339a87f2d54c` and builds Linux amd64 binaries. The smoke test verifies two synthetic users and binds one generated device secret to each OpenPak account's `roomcircuit-lab` identity. The private game callback checks current account status and the active binding before returning an identity. `local-receipt.json` contains no credentials.

The game runner can consume `.runtime/game-test.json` while the lab is active. A game ticket lasts up to 30 minutes and is lost if the identity process stops. The account session and device binding remain in PostgreSQL. An authenticated device can obtain a fresh ticket using the same device secret and gets the same player ID.

Run `python3 -B smoke.py --restart-proof` to verify that behavior for both users against the running local Compose project. It restarts the account and identity processes and invalidates any previous game's fixture tickets. `COMPOSE_PROJECT_NAME`, `RC_ACCOUNT_PORT` and `RC_IDENTITY_PORT` can select a separate local instance from a separate work directory. Stop the containers with `sh lab.sh down`. Keep `.runtime/lab.env` with the database volume. It holds the PostgreSQL password and the keys needed to open existing sessions and authenticate internal requests. The lab files holding keys and game fixtures are owner-only.

| Status | Meaning |
| --- | --- |
| `400` | Malformed request or invalid device secret. |
| `401` | Missing or invalid web session, internal key or game ticket. |
| `403` | Account unverified or disabled, or the device binding is no longer active. |
| `404` | No active game ticket for that player ID. |
| `409` | Device already belongs to another account. |
| `429` | The local fixture ticket limit was reached. |
| `503` | The account core or its linked state is unavailable. |

The test client supplies a synthetic device secret and an existing verified web session directly. A player-facing device login would need its own short-lived device-code approval flow and secure device credential storage. This lab does not exercise physical-device provisioning, official platform sign-in, game ownership, or middleware configuration for real titles.
