#!/usr/bin/env python3
"""Test independent HTTPS browser approval against the local account core."""
import http.cookies
import importlib.util
import json
from pathlib import Path
import re
import secrets
import ssl
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPSHandler, ProxyHandler, Request, build_opener


ROOT = Path(__file__).resolve().parent
LAB = ROOT / '.runtime'
ORIGIN = 'https://127.0.0.1:39280'
PRIVATE = 'http://127.0.0.1:39281'
SCOPE = 'roomcircuit-game'
ACCOUNT_APP = ROOT.parent / 'account-lab' / 'smoke.py'
spec = importlib.util.spec_from_file_location('account_smoke', ACCOUNT_APP)
account = importlib.util.module_from_spec(spec)
spec.loader.exec_module(account)


def create_account(role, country):
    suffix = secrets.token_hex(4)
    auth = {'email': f'{role.lower()}.{suffix}@example.test', 'password': secrets.token_hex(20)}
    registration, _ = account.call(account.ACCOUNT, '/v1/register', {**auth, 'display_name': role,
        'handle': role[:5] + suffix, 'country': country, 'birthday': '1994-01-01'}, expected=201)
    account.call(account.ACCOUNT, '/v1/verify?' + urlencode({'token': registration['verification_token']}))
    return auth, registration['account_id']


def main():
    if not __debug__:
        raise RuntimeError('Run the smoke test with assertions enabled')
    ca = LAB / 'ca.pem'
    context = ssl.create_default_context(cafile=str(ca))
    client = build_opener(ProxyHandler({}), HTTPSHandler(context=context))
    key = (LAB / 'game-key').read_text().strip()
    internal = {'X-Internal-Key': key}
    def browser(path, data=None, headers=None, expected=200, form=False):
        url = ORIGIN + path
        payload = (urlencode(data) if form else json.dumps(data)).encode() if data is not None else None
        content_type = 'application/x-www-form-urlencoded' if form else 'application/json'
        request = Request(url, payload, {'Content-Type': content_type, **(headers or {})})
        try:
            result = client.open(request, timeout=8)
        except HTTPError as error:
            result = error
        with result:
            assert result.status == expected, f'{path.split("?")[0]}: got {result.status}, expected {expected}'
            value = result.read().decode()
            return value if 'text/html' in result.headers.get('Content-Type', '') else json.loads(value), result.headers

    account.ready(account.ACCOUNT, '/healthz')
    account.ready(PRIVATE, '/healthz')
    browser('/healthz')
    a, owner_a = create_account('NativeHost', 'US')
    b, owner_b = create_account('NativeGuest', 'GB')
    _, session = account.call(account.ACCOUNT, '/v1/login', a)
    legacy, _ = account.call(account.IDENTITY, '/v1/devices/link', {'device_key': secrets.token_hex(32)},
        {'Authorization': 'Bearer ' + session['openpak_session']}, expected=201)
    account.write_fixture(LAB / 'browser-test.json', {'issuer': ORIGIN, 'ca_file': str(ca),
        'host': a, 'guest': b, 'RC_IDENTITY_URL': PRIVATE, 'RC_IDENTITY_KEY': key,
        'legacy_token': legacy['game_token']})

    def device(device_key):
        item, _ = browser('/v1/device/authorize', {'device_key': device_key, 'scope': SCOPE}, expected=201)
        assert item['verification_uri_complete'].startswith(ORIGIN + '/device?') and item['interval'] == 2
        return item

    def approval(item, player, decision='approve', expected=200, bad_cookie=False):
        page, headers = browser('/device?' + urlencode({'user_code': item['user_code']}))
        csrf = re.search(r'name="csrf_token" value="([a-f0-9]+)"', page)[1]
        cookies = http.cookies.SimpleCookie(headers['Set-Cookie'])
        assert cookies['rc_device_form']['secure'] and cookies['rc_device_form']['httponly']
        cookie = 'rc_device_form=' + ('wrong' if bad_cookie else cookies['rc_device_form'].value)
        result, _ = browser('/device', {'user_code': item['user_code'], 'csrf_token': csrf, **player,
            'decision': decision}, {'Cookie': cookie, 'Origin': ORIGIN}, expected=expected, form=True)
        return result

    def poll(item, expected=200):
        value, _ = browser('/v1/device/token', {'device_code': item['device_code']}, expected=expected)
        return value

    a_key, b_key = secrets.token_hex(32), secrets.token_hex(32)
    a_device, b_device = device(a_key), device(b_key)
    assert poll(a_device, 400)['error'] == 'authorization_pending'
    assert poll(a_device, 400)['error'] == 'slow_down'
    assert approval(a_device, a, expected=403, bad_cookie=True)['error'] == 'invalid_browser_session'
    assert approval(a_device, {**a, 'password': 'bad password'}, expected=401)['error'] == 'invalid_credentials'
    assert approval(a_device, a)['status'] == 'approved'
    assert approval(b_device, b)['status'] == 'approved'
    time.sleep(a_device['interval'] + 0.1)
    first, second = poll(a_device), poll(b_device)
    assert first['pid'] != second['pid'] and owner_a != owner_b
    assert poll(a_device, 400)['error'] == 'expired_token'
    rejected = device(secrets.token_hex(32))
    assert approval(rejected, b, decision='deny')['status'] == 'denied'
    assert poll(rejected, 400)['error'] == 'access_denied'
    cross = device(a_key)
    assert approval(cross, b, expected=409)['error'] == 'device_link_denied'

    path = '/internal/switch/identity'
    for ticket, identity in ((first, owner_a), (second, owner_b)):
        assert ticket['token_type'] == 'Bearer' and ticket['scope'] == SCOPE and ticket['expires_in'] > 0
        assert ticket['id_token'] != ticket['access_token']
        result, _ = account.call(PRIVATE, path, {'id_token': ticket['id_token']}, internal)
        assert result['pid'] == ticket['pid'] and result['account_id'] == identity
        account.call(PRIVATE, path, {'id_token': ticket['access_token']}, internal, expected=401)
    account.call(PRIVATE, path, {'id_token': first['id_token']}, {'X-Internal-Key':'wrong'}, expected=401)
    account.call(PRIVATE, path, {'id_token': legacy['game_token']}, internal, expected=401)
    account.call(PRIVATE, path, {'id_token': first['id_token'], 'application_id':'0000000000001234'}, internal, expected=403)
    token = first['id_token']
    half = len(token)//2
    tampered = token[:half] + ('A' if token[half] != 'A' else 'B') + token[half+1:]
    account.call(PRIVATE, path, {'id_token':tampered}, internal, expected=401)
    account.write_fixture(LAB / 'game-test.json', {'RC_IDENTITY_URL': PRIVATE, 'RC_IDENTITY_KEY': key,
        'RC_HOST_TOKEN': first['id_token'], 'RC_GUEST_TOKEN': second['id_token'],
        'RC_HOST_PID': first['pid'], 'RC_GUEST_PID': second['pid']})
    receipt = {'owned_browser_signins':2,'owned_device_grants':2,'rsa_verified_game_tokens':2,
        'browser_csrf':True,'pending_and_denial':True,'wrong_account_and_wrong_application':True,
        'other_issuer_and_bad_signature_rejected':True,'access_token_rejected_for_game_identity':True,
        'tls_ca_validation':True,'real_console_tested':False}
    (ROOT / 'local-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('Two approved browser grants and signed game identities verified through the local account core.')


if __name__ == '__main__':
    main()
