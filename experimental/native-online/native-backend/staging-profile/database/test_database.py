#!/usr/bin/env python3
"""Exercise pg-stage only inside a disposable PostgreSQL 16 container."""
import os
from pathlib import Path
import secrets
import subprocess
import time

SCRIPT = Path(__file__).resolve().parent / 'bootstrap.sh'
NAME = 'roomcircuit-pg-stage-test-' + secrets.token_hex(5)
PORT = '39900'
ROOT = '/tmp/roomcircuit-private-stage'
ENV = ['RC_STAGE_PG_BIN=/usr/lib/postgresql/16/bin', 'RC_STAGE_PG_ROOT=' + ROOT, 'RC_STAGE_PG_PORT=' + PORT]


def run(*args, input=None, expect=0, timeout=90):
    result = subprocess.run(args, input=input, capture_output=True, text=True, timeout=timeout)
    if result.returncode != expect:
        # SQL, secrets and command arguments are deliberately not logged.
        raise RuntimeError('isolated command failed: ' + args[0] + ' status=' + str(result.returncode))
    return result.stdout


def inside(*args, expect=0, input=None):
    return run('docker', 'exec', '-i', '--user', 'postgres', NAME, *args, expect=expect, input=input)


def stage(action, expect=0, root=ROOT):
    env = [v if not v.startswith('RC_STAGE_PG_ROOT=') else 'RC_STAGE_PG_ROOT=' + root for v in ENV]
    return inside('env', *env, '/bin/sh', '/bootstrap.sh', action, expect=expect)


def psql(sql, expect=0, host='127.0.0.1', database='roomcircuit_stage'):
    command = 'export PGPASSWORD="$(cat "$1/state/postgres/app.pass")"; exec "$2/psql" -X -v ON_ERROR_STOP=1 -h "$3" -p "$4" -U rcstage -d "$5" -At'
    return inside('/bin/sh', '-c', command, 'test', ROOT, '/usr/lib/postgresql/16/bin', host, PORT, database, input=sql + '\n', expect=expect)


def main():
    try:
        run('docker', 'run', '-d', '--name', NAME, '--network', 'none', '--user', 'postgres',
            '-v', str(SCRIPT) + ':/bootstrap.sh:ro', 'postgres:16', 'sleep', 'infinity')
        inside('mkdir', '-p', ROOT)
        stage('init')
        assert inside('stat', '-c', '%a', ROOT + '/env/database.env').strip() == '600'
        for path in ('admin.pass', 'app.pass', 'database.env'):
            assert inside('stat', '-c', '%a', ROOT + '/state/postgres/' + path).strip() == '600'
        original = inside('sha256sum', ROOT + '/env/database.env').split()[0]
        stage('init')
        assert inside('sha256sum', ROOT + '/env/database.env').split()[0] == original
        inside('env', *ENV, '/bin/sh', '-c', 'exec /bin/sh /bootstrap.sh serve >/tmp/rc-pg-stage.log 2>&1 &')
        for _ in range(80):
            ready = subprocess.run(['docker', 'exec', '--user', 'postgres', NAME, '/usr/lib/postgresql/16/bin/pg_isready', '-h', '127.0.0.1', '-p', PORT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            if ready.returncode == 0:
                break
            time.sleep(.25)
        else:
            raise RuntimeError('staging PostgreSQL did not become ready')
        assert '127.0.0.1' in psql('SHOW listen_addresses;')
        assert 'rcstage' in psql('SELECT current_user;')
        # The application account is not a database administrator.
        psql('CREATE ROLE must_fail;', expect=3)
        psql('SELECT 1;', expect=2, database='postgres')
        psql('CREATE TABLE rc_probe(value integer NOT NULL); INSERT INTO rc_probe VALUES(731);')
        psql('SELECT 1;', expect=2, host='127.0.0.2')
        # Verify the actual listener bytes in procfs, not just the configured value.
        listeners = inside('cat', '/proc/net/tcp')
        port_hex = f'{int(PORT):04X}'
        matches = [line.split()[1] for line in listeners.splitlines()[1:] if line.split()[1].endswith(':' + port_hex) and line.split()[3] == '0A']
        assert matches and all(value.startswith('0100007F:') for value in matches)
        # A second staging directory cannot seize the existing server's port.
        failure = '/tmp/roomcircuit-private-stage-failure'
        inside('mkdir', '-p', failure)
        stage('init', expect=1, root=failure)
        inside('test', '!', '-e', failure + '/state/postgres')
        inside('test', '!', '-e', failure + '/env/database.env')
        assert not inside('find', failure + '/state', '-maxdepth', '1', '-type', 'd', '-name', '.pg-init.*', '-print').strip()
        assert psql('SELECT value FROM rc_probe;').strip() == '731'
        # Concurrent initialization never overwrites or nests a second cluster.
        concurrent = '/tmp/roomcircuit-private-stage-concurrent'
        inside('mkdir', '-p', concurrent)
        args = ['docker', 'exec', '--user', 'postgres', NAME, 'env',
                'RC_STAGE_PG_BIN=/usr/lib/postgresql/16/bin',
                'RC_STAGE_PG_ROOT=' + concurrent, 'RC_STAGE_PG_PORT=39901',
                '/bin/sh', '/bootstrap.sh', 'init']
        first = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        second = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        results = [first.wait(timeout=90), second.wait(timeout=90)]
        assert 0 in results and all(code in (0, 1) for code in results)
        assert not inside('find', concurrent + '/state/postgres', '-maxdepth', '1', '-type', 'd', '-name', '.pg-init.*', '-print').strip()
        inside('cmp', '-s', concurrent + '/state/postgres/database.env', concurrent + '/env/database.env')
        # Restart the exact database and prove data and credentials persist.
        inside('/usr/lib/postgresql/16/bin/pg_ctl', '-D', ROOT + '/state/postgres/data', '-m', 'fast', '-w', 'stop')
        inside('env', *ENV, '/bin/sh', '-c', 'exec /bin/sh /bootstrap.sh serve >/tmp/rc-pg-stage.log 2>&1 &')
        for _ in range(80):
            ready = subprocess.run(['docker', 'exec', '--user', 'postgres', NAME, '/usr/lib/postgresql/16/bin/pg_isready', '-h', '127.0.0.1', '-p', PORT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            if ready.returncode == 0:
                break
            time.sleep(.25)
        else:
            raise RuntimeError('staging PostgreSQL did not restart')
        assert psql('SELECT value FROM rc_probe;').strip() == '731'
        assert inside('sha256sum', ROOT + '/env/database.env').split()[0] == original
        stage('init')
        # Recover a crash after atomic state commit and before env publication.
        inside('unlink', ROOT + '/env/database.env')
        stage('init')
        assert inside('sha256sum', ROOT + '/env/database.env').split()[0] == original
        # A conflicting env is not overwritten or used by a new initialization.
        inside('/bin/sh', '-c', 'printf bad > "$1"', 'test', ROOT + '/env/database.env')
        stage('init', expect=1)
        assert inside('cat', ROOT + '/env/database.env').strip() == 'bad'
        inside('cp', ROOT + '/state/postgres/database.env', ROOT + '/env/database.env')
        stage('init')
        admin = inside('cat', ROOT + '/state/postgres/admin.pass').strip()
        app = inside('cat', ROOT + '/state/postgres/app.pass').strip()
        logs = inside('cat', ROOT + '/state/postgres/postgres.log') + inside('cat', '/tmp/rc-pg-stage.log')
        assert admin not in logs and app not in logs
        print('PASS dedicated PostgreSQL: loopback binding, isolated role/database, persistent data, restart, idempotence and failure rollback')
    finally:
        subprocess.run(['docker', 'rm', '-f', NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)


if __name__ == '__main__':
    main()
