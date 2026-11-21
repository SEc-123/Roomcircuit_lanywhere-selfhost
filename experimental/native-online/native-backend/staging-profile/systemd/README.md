# Isolated staging unit templates

`render.py` writes seven Linux systemd service units and one stack target into a **new local output directory**. It does not install, enable, start or stop anything. The staging root must be a normalized absolute path containing only letters, digits, `.`, `_`, `-` and `/`; whitespace and systemd specifiers are rejected. It must be outside home directories hidden by `ProtectHome`. For actual systemd use place it under `/opt/` or `/var/lib/`; `/tmp` and `/var/tmp` are valid only for offline rendering tests because `PrivateTmp=yes` hides those paths from other units.

```sh
python3 render.py --stage /opt/roomcircuit-native-stage \
  --title mario-party-superstars --output /tmp/roomcircuit-stage-units
```

The generated units use the dedicated non-login user and group `roomcircuit-native-stage`. Create neither the user nor any unit on a host until the separate staging installer has verified ownership, free ports, runtime compatibility, rollback and isolation. The renderer does not perform those checks. Environment files live under `<stage>/env`, must be owned by this user and have mode 0600, and contain secrets; never publish them. Binaries and the independent PostgreSQL runtime live under `<stage>/bin` or an explicitly configured read-only runtime path. Persistent database and encrypted application state live under `<stage>/state`.

Before starting the database unit, run the independent `pg-stage init` step as the dedicated user. It creates a separate PostgreSQL 16 cluster and database. The database unit calls only `pg-stage serve`; it will not initialize or erase a database. Core startup reads the resulting `env/database.env` in addition to `env/core.env`. PostgreSQL may restore that env file after an interrupted initialization, so only its unit may write `<stage>/env`; the other services can write `<stage>/state` only.

Services have `After`/`Wants` ordering and retry on failure. This ordering is not a readiness check: the core and adapter may restart while dependencies initialize. Unit syntax and template tests do not prove systemd startup, restart ordering, transaction recovery, or persistent storage on a particular host.

All generated application listeners and the NEX advertised station are fixed to `127.0.0.1`. PostgreSQL defaults to `127.0.0.1:39900`; core uses 39910–39911, identity 39912, broker 39913–39914, adapter 39915–39916, and the staging-only registration gateway 39917. NEX uses the selected title's fixed port block. Check all ports against existing services before installing. The registration gateway injects a private internal credential and is for local staging only. It must never be exposed publicly. The adapter does not enroll a physical console.

Each unit asks systemd to deny IP traffic except localhost and restricts address families, but IP firewall directives depend on host kernel/cgroup support. Verify enforcement independently; never rely on those directives alone for isolation. The application binds themselves are also fixed to loopback. No public DNS, CA trust, firewall changes or device profile are installed.

## Resource budget

The seven unit `MemoryMax` values total **360 MiB**: PostgreSQL 72, username core 64, identity 32, broker 48, adapter 48, registration gateway 32 and one NEX process 64 MiB. The Go limits are below the matching cgroup caps. These limits are experimental; no production capacity or concurrent-player claim follows from them. Kernel, page cache, database initialization, builds, other workloads and cgroup accounting can consume additional memory.

A conservative initial preflight needs the capped 360 MiB **plus at least 128 MiB reserve**. The observed server snapshot had roughly 491 MiB available, leaving only about 3 MiB above that threshold; it also has no swap. Do not treat this snapshot as sufficient headroom. Re-measure before any deployment, review existing workload peaks, and run controlled real-process memory and failure tests. A cgroup OOM kill is a failed test, not capacity validation.

Run `python3 -B test_render.py` for path, dependency, no-overwrite, cleanup, scope and cap checks. Linux `systemd-analyze verify` can additionally check parser compatibility, but only a real Linux/systemd restart and recovery exercise validates supervisor behavior.
