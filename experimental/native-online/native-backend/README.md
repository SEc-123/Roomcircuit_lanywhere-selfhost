# RoomCircuit own-network device sign-in lab

The lab uses the same independent account core as `../account-lab`. A device requests a one-time code, a player approves it in a browser after signing in to that core, and the device receives two distinct signed tokens. The game callback verifies the issuer, identity-token purpose, audience, expiry, live approved grant and current account/device binding.

```sh
cd ../account-lab
sh build.sh
sh lab.sh up
cd ../native-backend
sh run.sh
```

Leave the service running. In another terminal, run `go test ./...` and `python3 -B smoke.py` from this directory. The browser test trusts the generated `.runtime/ca.pem`. Both listeners bind to `127.0.0.1`. The browser uses TLS on `39280`. The game callback requires its own service key on `39281`. The account core's private gRPC port is `39172` for the local Compose setup. The fixture runner writes its credentials with owner-only permissions to `.runtime/`.

| Endpoint | Purpose |
| --- | --- |
| `POST /v1/device/authorize` | Start a short-lived `roomcircuit-game` device request. The device keeps its secret and receives a private polling code and a public browser code. |
| `GET` and `POST /device` | Confirm the displayed code and authenticate the verified account. Approval records the actual account link. Denial issues no grant. |
| `POST /v1/device/token` | Poll every two seconds. A successful exchange consumes the code and issues distinct 15-minute access and signed ID tokens, plus a refresh credential. |
| `POST /v1/device/refresh` | Rotate the refresh credential and issue fresh tokens for a supported application. |
| `POST /v1/device/logout` | Revoke the device grant with its current refresh credential. |
| `POST /internal/switch/identity` | Accept the own ID token and verify the active account/device binding. A declared application ID must match between the grant and the request. |
| `POST /internal/grants/check` | Let the private BAAS adapter revalidate a previously recorded device grant for a supported application. |
| `POST /internal/account/profile` | Read the verified account profile using an access token scoped to the requested application. |

Device links live in PostgreSQL. Keep the broker's owner-only `RC_DEVICE_GRANT_STATE` file and issuer signing key across restarts. The store holds SHA-256 digests of rotating refresh credentials and checkpoints the account-core unlink event cursor with each revocation. An account link recreated with the same core ID needs fresh approval after an unlink. During bounded event catch-up, authorization retries with `503`. The account core, grant lookup, profile and game callback stay private.

A linked device can sign in again after its refresh credential expires. A revoked grant or disabled account is denied. Browser approval covers the configured RoomCircuit title directory and each credential carries its exact title scope.

The upstream `nx-baas` daemon has separate shop, Penne and parental-control services as mandatory startup dependencies. BCAT is optional. This lab uses the documented token-proof pattern and its own actual grant check. It does not launch the full daemon, issue platform entitlement, or enroll a physical console. The current HOS 19.0.1 console setup and a retail game's online-menu connection remain hardware acceptance steps.

## Username-only realm

The separate [username core](username-core/README.md) requires only a username and password. Select `RC_ACCOUNT_REALM=username` on the broker and identity bridge, and `NX_LAB_ACCOUNT_REALM=username` on the adapter. The broker validates the private HTTP and gRPC realm markers before accepting unverified, email-free profiles. Email-based realms retain their existing verification check.

Registration must use an independent HTTPS listener that preserves the client socket address. The current transparent SNI relay causes the broker to see a loopback peer, so it must not be used as the registration entry point. Forwarded IP headers are not trusted. Public listeners and certificates are not yet deployed or verified.
