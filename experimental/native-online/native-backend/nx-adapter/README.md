# Owned native-route lab

This is a small independent adapter based on the login shape in [OpenPak's `nx-baas` source](https://github.com/openpak/nx-baas/blob/cf93fd71d05296ac321a90fa880af707bf35ed44/internal/baas/baas.go#L281-L372). It uses the browser-approved application grant from the local native-backend service and the same OpenPak account database. Built with the Go standard library, it does not need to launch the full upstream `nx-baas` image.

1. Start `../run.sh`, then run `python3 -B ../smoke.py` to provision verified test accounts and the own TLS CA. Keep that service running.
2. Start `sh run.sh` from this directory. It generates its own distinct RSA signing key and private game-service key.
3. Run `python3 -B smoke.py`. The test client trusts the locally generated CA normally. It writes a private NEX fixture to `.runtime/game-test.json` and a credential-free `local-receipt.json`.

| Route | Evidence checked by the owned adapter |
| --- | --- |
| `POST /1.0.0/users` | A short-lived enrolled-device token provisions an unlinked device ID and password. The alternate fixture path uses an already-approved independent account grant. |
| `POST /1.0.0/federation` | The device password and verified account grant belonging to the same player. |
| `POST /1.0.0/login` | The matching device password and enrollment. A provisional login authorizes only the device's profile. A linked login also verifies the current application grant. |
| `GET /v1/users/{id}/membership` | A valid access token for that user and the requested application matching the active grant. |
| `GET /2.0.0/users/me` | The real profile from the independent account core, with a purpose-checked access token. |
| `GET /1.0.0/certificates` | Public keys for the owned network-account issuer. |
| BAAS certificate routes | Separate purposes and key IDs for the BAAS ID and access tokens. Their `jku` values point to the owned key origin. |
| Private `POST /internal/switch/identity` | A separate service key and a signed ID token admit a game player. An admitted game's private PID checks extend that connection's lease and revalidate the durable device grant without extending the original login token. |

The upstream full stack declares shop, Penne and parental controls mandatory and BCAT optional. This adapter returns `501` for unimplemented routes. HOS 19.0.1 device authentication, account profile and browser bootstrap still need hardware validation.

Device associations live in `.runtime/devices.enc` by default. The encrypted file contains password digests, owner IDs and the durable grant reference. Keep its owner-only `device-state-key` with the file. A restart preserves linked device IDs and accepts their existing passwords while the grant stays active. Game logins and admission leases are held in process memory. A game connected before a restart needs a fresh signed login afterward.

## Target-host loopback verification

Extract the deployment archive at the existing independent account lab's `$stage`. The native-backend service on `39280`/`39281` must already be running and its browser fixture present. The adapter reuses that host's local TLS certificate and CA for `39282`, and its own signed identity issuer remains separate. Neither the identity endpoint on `39283` nor the account endpoints are published.

```sh
adapter="$stage/native-backend/nx-adapter"
chmod 700 "$adapter/.bin/nx-adapter-durable-linux-amd64"
systemd-run --unit=roomcircuit-nx-adapter-lab \
  --property=WorkingDirectory="$adapter" --property=MemoryMax=64M \
  --property=NoNewPrivileges=yes \
  --setenv=NX_LAB_BINARY="$adapter/.bin/nx-adapter-durable-linux-amd64" \
  /bin/sh "$adapter/run.sh"
python3 -B "$adapter/smoke.py"
```

### HTTPS issuer and listener

`NX_LAB_ISSUER` is the owned HTTPS origin placed in signed tokens. `NX_LAB_LISTEN` chooses the address and port for the game-facing TLS listener. For example, an owned public HTTPS origin can be paired with a local bind behind its TLS proxy. `NX_LAB_CERT` and `NX_LAB_TLS_KEY` supply the listener's certificate pair. Private `NX_LAB_INTERNAL` and `NX_LAB_GRANT_URL` remain loopback. The default application ID is synthetic. Both listener ports default to loopback.

`NX_LAB_BAAS_KEY_ORIGIN` selects the owned BAAS key hostname. On a hostname separate from NA, the keys use `/1.0.0/certificates` and `/1.0.0/internal_certificates`. A same-host fixture uses `/1.0.0/baas_certificates` for its BAAS ID key. `NX_LAB_BAAS_AUDIENCE` optionally selects the registered 16-hex BAAS client audience. The adapter checks actual game title scope separately. Both linked BAAS tokens carry the device binding ID used in the profile ETag. A title login carries its verified application version and an authenticated ticket bound to the enrolled device.

Each `NX_LAB_APPLICATION` uses separate `NX_LAB_STATE` and `NX_LAB_STATE_KEY` files. Changing the issuer while keeping the same application preserves device associations. Changing applications requires its own store.

Run `python3 -B restart_test.py --binary .bin/nx-adapter-durable-linux-amd64` on the Linux target after preparing the adjacent browser fixture. It starts a disposable adapter with separate ports, verifies an issuer different from its bind address, and logs two-device recovery to `restart-receipt.json`. It leaves the existing running adapter and its devices intact. Omit `--binary` to build from source on the current host.

## Owned OAuth route check

The adapter exposes `/connect/1.0.0/authorize`, `/connect/1.0.0/api/session_token` and `/connect/1.0.0/api/token` for the registered own client. `NX_LAB_OAUTH_CLIENT_ID` and `NX_LAB_OAUTH_REDIRECT` select its exact callback. `NX_LAB_NA_ISSUER` selects the account HTTPS origin and `NX_LAB_NA_SIGNING_KEY` supplies an RSA key distinct from the BAAS issuer key. Preserve the key and the broker trust configuration across restarts. S256 proof enables one exchange of the authorization code. The account issuer signs distinct ID and access tokens and seals the broker refresh secret in a scoped rotating credential.

The native profile registers the documented NxAccount client and its Settings callback. The browser's `nx` scope can expand only to the documented profile fields. A supplied account-role DAuth proof is checked and binds the resulting authorization to that enrolled device. The loopback fixture keeps its own callback and explicit scopes. [Switch account exchange](https://github.com/kinnay/NintendoClients/wiki/Account-Server-(Switch)#methods)

Run `python3 -B oauth_test.py --binary .bin/nx-adapter-durable-linux-amd64` for the two-account check. It verifies the published RSA key, issuer, client audience, nonce, scope, access token hash, separate stable account and game IDs, and refresh after restart. `/connect/1.0.0/api/logout` revokes the owned grant. The browser form uses the owned account login and broker over its configured local HTTPS origin.

## Console device bootstrap

Set `NX_LAB_DEVICE_HOST` to a dedicated hostname covered by the server certificate and `NX_LAB_ENROLLED_SPKI` to the approved public client key fingerprints. The direct TLS listener verifies the enrolled key. The account and title listeners serve without a client-certificate prompt. DAUTH signs with its own `NX_LAB_DAUTH_SIGNING_KEY`; the launcher creates one on first use. Its public key is served at `/keys` on the owned HTTPS origin selected by `NX_LAB_DCERT_ORIGIN`.

The two HOS 19 account client IDs remain in their own role. The BAAS/AAUTH device role defaults to `8f849b5d34778d8e` and `NX_LAB_DEVICE_CLIENT_ID` can select an explicitly configured replacement. [DAuth client roles](https://github.com/kinnay/NintendoClients/wiki/DAuth-Server#known-client-ids)

HOS 19.0.1 uses the form-encoded DAuth v7 challenge and singular token routes. The adapter also supports the v8 batch fixture. Each challenge is single-use. A valid device proof can request title authorization or retry a BAAS exchange until it expires. A BAAS retry returns the same still-valid application token. A provisional device profile needs an independently approved player grant and a matching device token to federate. A server restart invalidates bootstrap tokens. Provisioned profiles and completed links persist, with only password hashes stored. [DAuth versions](https://github.com/kinnay/NintendoClients/wiki/DAuth-Server#system-versions)

`go test -mod=readonly -run TestNativeBootstrapWire` exercises these routes over an actual local HTTPS listener. The test proves the independent enrollment and wire contract with test certificates. It does not establish that HOS 19.0.1 supplies an eligible client certificate to the configured device-auth hostname.

## Profile and title authorization

The enrolled device can read and edit its own profile and avatar before linking. Federation keeps the BAAS user ID and device password and retires the provisional session. Profile state and avatar data use authenticated encryption. Membership and game access require a verified linked grant.

Set `NX_LAB_TITLE_OAUTH_CLIENT_ID` to the registered 16-digit hexadecimal client ID for the active title. `POST /v5/application_auth_token` serves the matching `npf<id>://auth` callback and issues a signed title token to the enrolled device. The linked device needs that token for its private game login. `NX_LAB_AAUTH_ORIGIN` selects the title issuer. The public key is served at `/keys` on the owned HTTPS origin selected by `NX_LAB_AAUTH_KEY_ORIGIN`. The system Settings app's registered client and callback are configured separately.

`go test -mod=readonly -run 'TestOwnedNativeProfileAndMembership|TestNativeApplicationAuthorization'` verifies owner access, revocation, saved profiles and title key binding. HOS 19.0.1 acceptance still requires the actual console to use the configured account and application endpoints.
