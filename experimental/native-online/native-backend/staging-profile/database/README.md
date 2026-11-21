# Dedicated PostgreSQL staging instance

`pg-stage` initializes **a new PostgreSQL 16 cluster**, staging database `roomcircuit_stage` and non-superuser application role `rcstage`. It requires a dedicated non-root service user and never connects to an existing database cluster. The instance listens on `127.0.0.1` only; PostgreSQL uses SCRAM authentication. The default port for the surrounding staging profile is 39900, but an explicit free port is required.

Place `bootstrap.sh` at `<stage>/bin/pg-stage`, provide the PostgreSQL 16 binaries explicitly, and run as the dedicated `roomcircuit-native-stage` user:

```sh
export RC_STAGE_PG_ROOT=/absolute/private/stage
export RC_STAGE_PG_BIN=/absolute/path/to/postgresql-16/bin
export RC_STAGE_PG_PORT=39900
<stage>/bin/pg-stage init
<stage>/bin/pg-stage serve
```

`init` creates state under `<stage>/state/postgres/` and publishes `<stage>/env/database.env` with `RC_USERNAME_DATABASE_URL`. The staging root must already exist and be writable by the service user. The administrator and application secrets and environment are owner-only; secrets are not written to command-line arguments. The script does not modify other staging environment files. A kernel `flock` serializes initialization and is released on process exit. If initialization fails before commit, its dedicated temporary state is removed. If an existing state or environment conflicts, it is preserved and startup fails. A missing environment file after state commit is regenerated from the existing private state without changing credentials. A different port requires an explicit migration, not a repeated init.

`serve` runs PostgreSQL in the foreground for a supervisor. `init` is an explicit provisioning step and must not be invoked automatically on every service restart. Retain the entire state and secrets in private backups; changing either password independently breaks recovery. The calling supervisor must provide any trusted shared-library path required by the chosen PostgreSQL distribution.

## Isolated verification

```sh
python3 -B test_database.py
```

This test creates a disposable `postgres:16` Docker container with `--network none`, then verifies the real database: role privileges, loopback-only bind, excluded `127.0.0.2`, data and credentials across restart, repeated init, conflicting environment preservation, port-conflict rollback, recovery of interrupted environment publication, private file permissions and absence of generated passwords in database logs. It removes its own container. It does not install a service or touch any existing database.

The script requires Linux `flock`, Perl and GNU coreutils. It fsyncs the committed credentials, configuration and parent directories; the recovery test simulates interrupted environment publication, not physical power loss. The test is a local development check, not evidence of installation or resource capacity on the target host. Production-ready activation still requires the supplied PostgreSQL binaries and libraries to be verified, staging user and paths to be provisioned, and memory usage to be measured under the intended workload.
