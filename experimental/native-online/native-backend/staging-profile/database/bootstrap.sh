#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Independent, loopback-only PostgreSQL 16 for the RoomCircuit staging realm.
set -eu
umask 077
fail() { printf '%s\n' "$1" >&2; exit 1; }
sync_paths() {
    # Flush named files and containing directories without exposing contents.
    perl -MIO::Handle -e 'for my $path (@ARGV) { open(my $f, "<", $path) or die; $f->sync or die; close($f) or die; }' "$@" >/dev/null 2>&1 || fail 'Could not sync dedicated database state'
}
[ "$#" -eq 1 ] || fail 'Usage: pg-stage init|serve'
: "${RC_STAGE_PG_BIN:?PostgreSQL binary directory required}"
: "${RC_STAGE_PG_ROOT:?Existing staging root required}"
: "${RC_STAGE_PG_PORT:?Dedicated staging port required}"
case "$RC_STAGE_PG_ROOT" in /*) ;; *) fail 'Staging root must be absolute';; esac
[ "$RC_STAGE_PG_ROOT" != / ] && [ -d "$RC_STAGE_PG_ROOT" ] && [ ! -L "$RC_STAGE_PG_ROOT" ] || fail 'Invalid staging root'
[ "$(id -u)" -ne 0 ] || fail 'PostgreSQL must run as the dedicated non-root staging user'
case "$RC_STAGE_PG_PORT" in ''|0*|*[!0-9]*) fail 'Invalid PostgreSQL port';; esac
[ "$RC_STAGE_PG_PORT" -gt 0 ] && [ "$RC_STAGE_PG_PORT" -le 65535 ] || fail 'Invalid PostgreSQL port'
root=$(CDPATH= cd "$RC_STAGE_PG_ROOT" && pwd -P)
for dir in "$root/state" "$root/env"; do
    [ ! -L "$dir" ] || fail 'Staging state and env cannot be symlinks'
    mkdir -p "$dir"
    [ -d "$dir" ] || fail 'Cannot create staging directory'
    chmod 700 "$dir"
done
bindir=$RC_STAGE_PG_BIN
case "$bindir" in /*) ;; *) fail 'PostgreSQL binary directory must be absolute';; esac
for binary in postgres initdb pg_ctl psql; do
    [ -x "$bindir/$binary" ] || fail 'PostgreSQL 16 runtime is incomplete'
    case "$("$bindir/$binary" --version)" in "$binary (PostgreSQL) 16."*) ;; *) fail 'Every database tool must be PostgreSQL 16';; esac
done
data="$root/state/postgres"
envfile="$root/env/database.env"
[ ! -L "$data" ] && [ ! -L "$envfile" ] || fail 'Existing database path must not be a symlink'

check_committed() {
    [ -d "$data/data" ] && [ -f "$data/data/PG_VERSION" ] && [ "$(cat "$data/data/PG_VERSION")" = 16 ] || fail 'Database state is incomplete'
    [ -f "$data/database.env" ] && [ -f "$data/admin.pass" ] && [ -f "$data/app.pass" ] && [ -f "$data/port" ] || fail 'Database credentials are incomplete'
    [ "$(cat "$data/port")" = "$RC_STAGE_PG_PORT" ] || fail 'Existing database uses a different staging port'
    if [ -e "$envfile" ]; then
        [ -f "$envfile" ] && cmp -s "$data/database.env" "$envfile" || fail 'Existing database environment differs; preserved'
        chmod 600 "$envfile"
        sync_paths "$envfile" "$root/env"
    else
        # A crash after data commit but before env publication is recoverable.
        temp_env=$(mktemp "$root/env/.database.env.XXXXXXXX")
        cp "$data/database.env" "$temp_env"
        chmod 600 "$temp_env"
        sync_paths "$temp_env"
        if ! ln "$temp_env" "$envfile"; then
            unlink "$temp_env"
            [ ! -L "$envfile" ] && [ -f "$envfile" ] && cmp -s "$data/database.env" "$envfile" || fail 'Database environment appeared; preserved'
        else
            unlink "$temp_env"
        fi
        sync_paths "$envfile" "$root/env"
    fi
}

case "$1" in
  serve)
    [ -d "$data" ] || fail 'Initialize the dedicated database before starting it'
    check_committed
    exec "$bindir/postgres" -D "$data/data" -h 127.0.0.1 -p "$RC_STAGE_PG_PORT" -c unix_socket_directories= -c log_statement=none -c log_min_error_statement=panic
    ;;
  init)
    # A kernel lock prevents concurrent initializers from both committing
    # state. It is released by the OS if an initializer crashes.
    command -v flock >/dev/null 2>&1 || fail 'flock is required for safe database initialization'
    [ ! -L "$root/state/.pg-stage.lock" ] || fail 'Invalid database lock path'
    [ ! -e "$root/state/.pg-stage.lock" ] || [ -f "$root/state/.pg-stage.lock" ] || fail 'Invalid database lock path'
    exec 9>> "$root/state/.pg-stage.lock"
    chmod 600 "$root/state/.pg-stage.lock"
    flock -n 9 || fail 'Another database initialization is running'
    if [ -e "$data" ]; then check_committed; printf '%s\n' 'Dedicated staging database already initialized'; exit 0; fi
    [ ! -e "$envfile" ] || fail 'Existing database environment preserved; no database was changed'
    work=$(mktemp -d "$root/state/.pg-init.XXXXXXXX")
    started=0
    cleanup() {
        result=$?
        trap - EXIT HUP INT TERM
        if [ "$started" = 1 ]; then "$bindir/pg_ctl" -D "$work/data" -m immediate -w stop >/dev/null 2>&1 || :; fi
        if [ -d "$work" ]; then
            case "$work" in "$root"/state/.pg-init.*) rm -rf -- "$work" ;; *) printf '%s\n' 'Temporary path preserved' >&2 ;; esac
        fi
        exit "$result"
    }
    trap cleanup EXIT
    trap 'exit 1' HUP INT TERM
    openssl rand -hex 32 > "$work/admin.pass"
    openssl rand -hex 32 > "$work/app.pass"
    [ "$(wc -c < "$work/admin.pass" | tr -d ' ')" = 65 ] && [ "$(wc -c < "$work/app.pass" | tr -d ' ')" = 65 ] || fail 'Secret generation failed'
    "$bindir/initdb" -D "$work/data" --username=rcstageadmin --pwfile="$work/admin.pass" --auth-local=scram-sha-256 --auth-host=scram-sha-256 --no-locale --encoding=UTF8 > "$work/init.log" 2>&1 || fail 'Dedicated PostgreSQL initialization failed'
    cat >> "$work/data/postgresql.conf" <<CONFIG
listen_addresses = '127.0.0.1'
port = $RC_STAGE_PG_PORT
unix_socket_directories = ''
password_encryption = 'scram-sha-256'
shared_buffers = '24MB'
max_connections = 20
log_statement = 'none'
log_min_error_statement = panic
CONFIG
    "$bindir/pg_ctl" -D "$work/data" -l "$work/postgres.log" -w start 9>&- >/dev/null 2>&1 || fail 'Dedicated PostgreSQL failed to start; check the port and runtime'
    started=1
    admin=$(cat "$work/admin.pass")
    app=$(cat "$work/app.pass")
    # The password is sent through stdin, not command-line arguments.
    if ! printf "CREATE ROLE rcstage LOGIN PASSWORD '%s';\nCREATE DATABASE roomcircuit_stage OWNER rcstage;\nREVOKE CONNECT ON DATABASE postgres FROM PUBLIC;\nREVOKE CONNECT ON DATABASE template1 FROM PUBLIC;\nREVOKE ALL ON DATABASE roomcircuit_stage FROM PUBLIC;\n" "$app" |
        PGPASSWORD="$admin" "$bindir/psql" -X -v ON_ERROR_STOP=1 -h 127.0.0.1 -p "$RC_STAGE_PG_PORT" -U rcstageadmin -d postgres >/dev/null 2>&1; then
        fail 'Could not create the dedicated staging database'
    fi
    # Check the application's own password and database before committing state.
    if ! PGPASSWORD="$app" "$bindir/psql" -X -v ON_ERROR_STOP=1 -h 127.0.0.1 -p "$RC_STAGE_PG_PORT" -U rcstage -d roomcircuit_stage -c 'SELECT 1' >/dev/null 2>&1; then
        fail 'Dedicated application database authentication failed'
    fi
    "$bindir/pg_ctl" -D "$work/data" -m fast -w stop >/dev/null 2>&1 || fail 'Could not stop the initial database'
    started=0
    printf '%s\n' "$RC_STAGE_PG_PORT" > "$work/port"
    printf 'RC_USERNAME_DATABASE_URL=postgres://rcstage:%s@127.0.0.1:%s/roomcircuit_stage?sslmode=disable\n' "$app" "$RC_STAGE_PG_PORT" > "$work/database.env"
    unset admin app
    chmod 600 "$work/admin.pass" "$work/app.pass" "$work/database.env" "$work/port"
    sync_paths "$work/admin.pass" "$work/app.pass" "$work/database.env" "$work/port" "$work/data/postgresql.conf" "$work/data" "$work"
    # No data, log or environment is overwritten if the target appears.
    [ ! -e "$data" ] && [ ! -L "$data" ] || fail 'Database state appeared; preserved'
    # GNU mv -T avoids nesting inside an existing destination. -n prevents
    # overwriting even if another actor creates the destination after the check.
    mv -T -n -- "$work" "$data"
    [ ! -e "$work" ] || fail 'Database state appeared; preserved'
    work=''
    sync_paths "$root/state"
    check_committed
    printf '%s\n' 'Dedicated staging database initialized'
    ;;
  *) fail 'Usage: pg-stage init|serve';;
esac
