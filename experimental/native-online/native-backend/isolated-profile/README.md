# Isolated username and NEX profile

This profile starts disposable PostgreSQL and the username core, identity bridge, broker, adapter and hardened NEX runtime on the local machine. All account/game listeners bind to loopback; it never changes the existing public service or a Switch. It uses temporary generated TLS certificates and short-lived test credentials. The database, keys and processes are removed after the run.

Prepare an OpenPak Switch NEX v0.3.45 source checkout at commit `2af4741d321f9d9f9aa45f96454137f86130c288` and its Go module dependencies. Then:

```sh
cd experimental/native-online/native-backend
python3 -B username-core/e2e.py --nex-source /path/to/switch-nex --title mario-party-superstars
python3 -B username-core/e2e.py --nex-source /path/to/switch-nex --title pico-park
```

Each run registers two username-only players, approves and binds them, checks 20 approvals leave no temporary sessions, and uses their real test grants against the selected game's actual NEX profile. It checks the other game's endpoint rejects that title's grant. It also tests secure-ticket admission, reconnect after restart and a live connection's periodic authorization recheck against a controlled local authority. The selected game's configured auth and secure ports and temporary certificate are exercised. Broker, identity and core restart/revocation checks are local test operations. This is a protocol-client test; actual console login, peer traffic and gameplay remain unverified.

The `--nex-source` input is checked by the runtime builder against pinned upstream source file hashes. Ports are fixed by the game profiles and must be free. `go mod download` in that source checkout is required before running if dependencies are not already cached.

Public deployment additionally needs a domain and appropriately scoped trusted certificates, safe direct registration ingress that preserves the real source IP, verified per-device enrollment and game routing, and explicit isolation from sysMMC. This profile supplies none of those public services. No email is needed; password recovery by email is unavailable.
