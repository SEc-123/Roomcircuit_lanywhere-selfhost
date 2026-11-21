# Private native staging profile

This prepares one experimental username-only account and game stack. It does not install, enable, start, stop or expose any service. It does not modify an existing RoomCircuit service or a Switch.

On a build machine with Go 1.26, OpenSSL, cached module dependencies and the pinned OpenPak Switch NEX v0.3.45 source, create a **new** private output directory:

```sh
python3 -B prepare.py --root /private/build-output --name roomcircuit-stage \
  --nex-source /path/to/switch-nex --title mario-party-superstars \
  --pg-bin /opt/trusted-postgresql-16/bin \
  --pg-lib /opt/trusted-postgresql-16/lib
```

Repeat `--pg-lib` for every required trusted PostgreSQL library directory. The tool builds Linux x86-64 binaries, creates private keys and an isolated two-day loopback TLS certificate, generates environment files and renders seven systemd services plus a target. A single title is selected; game and account listeners are fixed to loopback. A newly generated profile has **no enrolled physical device**. It is not a public endpoint or an installable console package.

Before any target-host activation, verify the PostgreSQL 16 binaries and libraries, create the dedicated non-login service user, provision the new stage outside `/tmp` and home directories, verify ownership and permissions, and check every listed port. Database initialization is a separate explicit non-root step; see [database](database/README.md). The generated `env/database.env` is absent until initialization succeeds. Rendered units are not installed or started. Existing database state or credentials are never overwritten by a repeated initialization.

Retain private environment files, keys, encrypted application state and the separate PostgreSQL cluster together in access-controlled backups. On rollback, stop only this stage's services and preserve their state; the scripts do not alter the existing service. A trusted CA and secure public routing are separate deployment work. The temporary CA and local registration gateway must not be used as public infrastructure.

## Validation and limits

```sh
python3 -B systemd/test_render.py
python3 -B database/test_database.py
(cd register-gateway && go test -race ./...)
RC_NEX_SOURCE=/path/to/switch-nex python3 -B test_prepare.py
```

The database test uses a disposable, network-isolated PostgreSQL 16 container. The complete generator test builds binaries and checks hashes, private permissions and no-overwrite behavior. Unit syntax was checked using `systemd-analyze verify` in an isolated Ubuntu environment. These checks do not establish a real systemd boot, service dependency recovery, target-host capacity, or native Switch compatibility.

The unit `MemoryMax` limits total 360 MiB for one game. At least another 128 MiB of margin is required even for a preliminary staging review. A previous target snapshot had about 491 MiB available and no swap, leaving only 3 MiB over that threshold. This is insufficient evidence of safe capacity: measure real peak memory and concurrency without risking the existing service before deployment.

Device enrollment remains unresolved: the adapter currently accepts only explicitly configured SPKI fingerprints. Whether the active emuMMC exposes an appropriate client certificate and how to approve it safely must be verified on that device. It is possible to design software-assisted enrollment, but no working physical-device enrollment has been demonstrated. Do not change sysMMC or assume a copied console identity is available.
