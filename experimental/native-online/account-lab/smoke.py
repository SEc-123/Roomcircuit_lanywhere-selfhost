#!/usr/bin/env python3
"""Exercise only local, synthetic accounts and issue short-lived game fixtures."""
import argparse
import datetime
import http.cookiejar
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, build_opener, ProxyHandler


ROOT = Path(__file__).resolve().parent


def loopback_url(variable, fallback):
    port = int(os.environ.get(variable, fallback))
    if not 1 <= port <= 65535:
        raise ValueError(f'{variable} is outside the TCP port range')
    return f'http://127.0.0.1:{port}'


ACCOUNT = loopback_url('RC_ACCOUNT_PORT', 39170)
IDENTITY = loopback_url('RC_IDENTITY_PORT', 39171)
IDENTITY_PATH = '/internal/switch/identity'
HTTP = build_opener(ProxyHandler({}))


def call(base, path, data=None, headers=None, expected=200):
    payload = json.dumps(data).encode() if data is not None else None
    request = Request(base + path, payload, {'Content-Type': 'application/json', **(headers or {})})
    try:
        response = HTTP.open(request, timeout=8)
    except HTTPError as error:
        response = error
    with response:
        if response.status != expected:
            raise AssertionError(f'{path.split("?")[0]} returned {response.status}, expected {expected}')
        result = json.load(response)
        cookie = http.cookiejar.CookieJar()
        cookie.extract_cookies(response, request)
        return result, {item.name: item.value for item in cookie}


def ready(base, route):
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        try:
            call(base, route)
            return
        except (OSError, AssertionError):
            time.sleep(0.5)
    raise RuntimeError(f'Local service did not become ready at {base}')


def register(prefix, country):
    suffix = secrets.token_hex(4)
    credentials = {'email': f'{prefix.lower()}.{suffix}@example.test', 'password': secrets.token_hex(20)}
    profile = {**credentials, 'display_name': f'{prefix} fixture', 'handle': prefix + suffix,
               'country': country, 'birthday': '1994-01-01'}
    created, _ = call(ACCOUNT, '/v1/register', profile, expected=201)
    assert created['account_id'] and created['verification_token']
    device = {'device_key': secrets.token_hex(32)}
    call(IDENTITY, '/v1/devices/link', device, expected=401)
    # A valid web session is still insufficient until email verification.
    _, cookies = call(ACCOUNT, '/v1/login', credentials)
    auth = {'Authorization': 'Bearer ' + cookies['openpak_session']}
    call(IDENTITY, '/v1/devices/link', device, auth, expected=403)
    call(ACCOUNT, '/v1/verify?' + urlencode({'token': created['verification_token']}))
    call(ACCOUNT, '/v1/login', {**credentials, 'password': 'wrong password'}, expected=401)
    login, cookies = call(ACCOUNT, '/v1/login', credentials)
    auth = {'Authorization': 'Bearer ' + cookies['openpak_session']}
    who, _ = call(ACCOUNT, '/v1/whoami', headers=auth)
    assert who['email_verified'] and who['status'] == 'active' and who['account_id'] == login['account_id'] == created['account_id']
    issued, _ = call(IDENTITY, '/v1/devices/link', device, auth, expected=201)
    retry, _ = call(IDENTITY, '/v1/devices/link', device, auth, expected=201)
    assert retry['pid'] == issued['pid'] and retry['game_token'] != issued['game_token']
    return who, auth, device, retry


def write_fixture(path, body):
    with tempfile.NamedTemporaryFile('w', dir=path.parent, delete=False) as stream:
        staging = Path(stream.name)
        json.dump(body, stream)
    try:
        os.replace(staging, path)
    finally:
        staging.unlink(missing_ok=True)


def verify_game_identity(owner, ticket, country, game_auth):
    identity, _ = call(IDENTITY, IDENTITY_PATH, {'id_token': ticket['game_token']}, game_auth)
    assert identity == {'pid': ticket['pid'], 'baas_user_id': f"{ticket['pid']:016x}",
                        'nickname': owner['display_name'], 'country': country,
                        'account_id': owner['account_id'], 'friends': []}
    by_pid, _ = call(IDENTITY, IDENTITY_PATH, {'pid': ticket['pid']}, game_auth)
    assert by_pid == identity


def check_restart(players, game_auth, original_env):
    subprocess.run(['sh', str(ROOT / 'lab.sh'), 'restart'], check=True)
    ready(ACCOUNT, '/healthz')
    ready(IDENTITY, '/healthz')
    assert (ROOT / '.runtime/lab.env').read_bytes() == original_env

    for _, _, _, old in players:
        call(IDENTITY, IDENTITY_PATH, {'id_token': old['game_token']}, game_auth, expected=401)
        call(IDENTITY, IDENTITY_PATH, {'pid': old['pid']}, game_auth, expected=404)

    # Before issuing any new tickets, confirm both original device ownership
    # records survived. A stable derived PID alone cannot show persistence.
    call(IDENTITY, '/v1/devices/link', players[0][2], players[1][1], expected=409)
    call(IDENTITY, '/v1/devices/link', players[1][2], players[0][1], expected=409)
    renewed = []
    for (owner, auth, device, old), country in zip(players, ('US', 'GB')):
        # Use the pre-restart web session. Re-registering or signing in again
        # would hide lost database/session state.
        who, _ = call(ACCOUNT, '/v1/whoami', headers=auth)
        assert who['account_id'] == owner['account_id'] and who['status'] == 'active' and who['email_verified']
        ticket, _ = call(IDENTITY, '/v1/devices/link', device, auth, expected=201)
        assert ticket['pid'] == old['pid'] and ticket['game_token'] != old['game_token']
        verify_game_identity(owner, ticket, country, game_auth)
        renewed.append(ticket)

    return renewed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--restart-proof', action='store_true', help='Restart the local account and identity services and verify recovery.')
    args = parser.parse_args()
    if not __debug__:
        raise RuntimeError('Run the smoke test with Python assertions enabled')
    env_path = ROOT / '.runtime/lab.env'
    if env_path.is_symlink() or env_path.stat().st_mode & 0o077:
        raise RuntimeError('The lab environment file must be owner-only')
    original_env = env_path.read_bytes()
    env = dict(line.split('=', 1) for line in original_env.decode().splitlines())
    key = env['RC_IDENTITY_KEY']
    game_auth = {'X-Internal-Key': key}
    ready(ACCOUNT, '/healthz')
    ready(IDENTITY, '/healthz')
    players = (register('Host', 'US'), register('Guest', 'GB'))
    host, _, host_device, a = players[0]
    guest, guest_auth, _, b = players[1]
    assert host['account_id'] != guest['account_id'] and a['pid'] != b['pid']
    call(IDENTITY, '/v1/devices/link', host_device, guest_auth, expected=409)
    call(IDENTITY, IDENTITY_PATH, {'id_token': a['game_token']}, {'X-Internal-Key': 'wrong'}, expected=401)
    call(IDENTITY, IDENTITY_PATH, {'id_token': secrets.token_hex(32)}, game_auth, expected=401)
    call(IDENTITY, IDENTITY_PATH, {'pid': 1}, game_auth, expected=404)
    verify_game_identity(host, a, 'US', game_auth)
    verify_game_identity(guest, b, 'GB', game_auth)
    if args.restart_proof:
        a, b = check_restart(players, game_auth, original_env)
    write_fixture(ROOT / '.runtime/game-test.json', {'RC_IDENTITY_URL': IDENTITY, 'RC_IDENTITY_KEY': key,
        'RC_HOST_TOKEN': a['game_token'], 'RC_GUEST_TOKEN': b['game_token'],
        'RC_HOST_PID': a['pid'], 'RC_GUEST_PID': b['pid']})
    result = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'synthetic_accounts': 2,
              'register_verify_login': True, 'device_binding_and_claim_conflict': True,
              'positive_game_identity_and_isolation': True, 'independent_network': True}
    if args.restart_proof:
        result['process_restart'] = {'retained_web_sessions': 2, 'stable_device_identities': 2,
                                     'invalidated_game_tickets': 2, 'reissued_game_tickets': 2,
                                     'retained_internal_keys': True, 'cross_account_claim_rejected': True}
    (ROOT / 'local-receipt.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Two verified accounts, two isolated device bindings, and game-token identity checks passed.')


if __name__ == '__main__':
    main()
