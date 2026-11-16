#!/usr/bin/env python3
"""Test owned native wire-route semantics with two browser-approved test accounts."""
import base64
import http.cookies
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import ssl
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPSHandler, ProxyHandler, Request, build_opener


ROOT = Path(__file__).resolve().parent
LAB = ROOT / '.runtime'
WEB = 'https://127.0.0.1:39280'
NX = 'https://127.0.0.1:39282'
PRIVATE = 'http://127.0.0.1:39283'
APP = os.environ.get('NX_LAB_APPLICATION', '0000000000001234')
account_script = ROOT.parents[1] / 'account-lab' / 'smoke.py'
spec = importlib.util.spec_from_file_location('account_smoke', account_script)
account = importlib.util.module_from_spec(spec)
spec.loader.exec_module(account)


def main(restart=None, expected_issuer=None, receipt_path=None):
    if not __debug__:
        raise RuntimeError('Run the smoke test with assertions enabled')
    browser_fixture = json.loads((ROOT.parent / '.runtime/browser-test.json').read_text())
    trust = ssl.create_default_context(cafile=str(ROOT.parent / '.runtime/ca.pem'))
    client = build_opener(ProxyHandler({}), HTTPSHandler(context=trust))
    game_key = (LAB / 'game-key').read_text().strip()
    internal = {'X-Internal-Key': game_key}

    def send(base, path, data=None, headers=None, expected=200, form=False):
        payload = (urlencode(data) if form else json.dumps(data)).encode() if data is not None else None
        mime = 'application/x-www-form-urlencoded' if form else 'application/json'
        req = Request(base + path, payload, {'Content-Type': mime, **(headers or {})})
        try:
            res = client.open(req, timeout=10)
        except HTTPError as error:
            res = error
        with res:
            assert res.status == expected, f'{path.split("?")[0]} returned {res.status}, expected {expected}'
            body = res.read().decode()
            return (body if 'text/html' in res.headers.get('Content-Type', '') else json.loads(body)), res.headers

    def approved_grant(credentials):
        start, _ = send(WEB, '/v1/device/authorize', {'device_key': secrets.token_hex(32),
            'scope': 'roomcircuit-game', 'application_id': APP}, expected=201)
        page, headers = send(WEB, '/device?' + urlencode({'user_code': start['user_code']}))
        csrf = re.search(r'name="csrf_token" value="([a-f0-9]+)"', page)[1]
        token_cookie = http.cookies.SimpleCookie(headers['Set-Cookie'])['rc_device_form'].value
        got, _ = send(WEB, '/device', {'user_code': start['user_code'], 'csrf_token': csrf, **credentials,
            'decision': 'approve'}, {'Cookie': 'rc_device_form=' + token_cookie, 'Origin': WEB}, form=True)
        assert got['status'] == 'approved'
        result, _ = send(WEB, '/v1/device/token', {'device_code': start['device_code']})
        assert result['id_token'] != result['access_token']
        return result

    account.ready(PRIVATE, '/healthz')
    host, guest = (approved_grant(browser_fixture[role]) for role in ('host', 'guest'))
    assert host['pid'] != guest['pid']
    send(NX, '/1.0.0/users', {}, expected=401)
    send(NX, '/1.0.0/users', {}, {'Authorization': 'Bearer ' + host['access_token']}, expected=401)
    path = '/internal/switch/identity'
    sessions = []
    persisted_devices = []
    for own, other in ((host, guest), (guest, host)):
        created, _ = send(NX, '/1.0.0/users', {}, {'Authorization': 'Bearer ' + own['id_token']}, expected=201)
        device = created['deviceAccounts'][0]
        assert device['id'] and device['password']
        send(NX, '/1.0.0/login', device, expected=403, form=True)
        account.call(PRIVATE, path, {'pid': own['pid']}, internal, expected=404)
        send(NX, '/1.0.0/login', {**device, 'password': 'wrong'}, expected=400, form=True)
        send(NX, '/1.0.0/federation', {**device, 'idToken': other['id_token']}, expected=403, form=True)
        federation, _ = send(NX, '/1.0.0/federation', {**device, 'idToken': own['id_token']}, form=True)
        session, _ = send(NX, '/1.0.0/login', device, form=True)
        assert set(session) == {'accessToken','expiresIn','idToken','tokenType','user'}
        assert session['tokenType'] == 'Bearer' and session['expiresIn'] == 300
        assert session['idToken'] != session['accessToken'] and session['idToken'] != federation['idToken']
        assert session['user']['deviceAccounts'] == [{'id': device['id']}]

        membership = f"/v1/users/{session['user']['id']}/membership"
        auth = {'Authorization': 'Bearer ' + session['accessToken']}
        send(NX, membership + '?' + urlencode({'application_id': APP}), expected=401)
        yes, _ = send(NX, membership + '?' + urlencode({'application_id': APP}), headers=auth)
        no, _ = send(NX, membership, headers=auth)
        assert yes['membership']['active'] is True and no['membership']['active'] is False
        send(NX, '/v1/users/other/membership?' + urlencode({'application_id': APP}), headers=auth, expected=403)

        value, _ = account.call(PRIVATE, path, {'id_token': session['idToken']}, internal)
        assert value['pid'] == own['pid'] and value['application_id'] == APP and value['baas_user_id'] == session['user']['id']
        connected, _ = account.call(PRIVATE, path, {'pid': own['pid'], 'application_id': APP}, internal)
        assert connected == value
        account.call(PRIVATE, path, {'pid': own['pid']}, {'X-Internal-Key': 'wrong'}, expected=401)
        account.call(PRIVATE, path, {'pid': own['pid'], 'application_id': 'unrelated'}, internal, expected=403)
        account.call(PRIVATE, path, {'pid': own['pid'], 'id_token': session['idToken']}, internal, expected=400)
        account.call(PRIVATE, path, {'pid': (1 << 63) - 1}, internal, expected=404)
        account.call(PRIVATE, path, {'id_token': session['accessToken']}, internal, expected=401)
        account.call(PRIVATE, path, {'id_token': own['id_token']}, internal, expected=401)
        account.call(PRIVATE, path, {'id_token': session['idToken']}, {'X-Internal-Key': 'wrong'}, expected=401)
        account.call(PRIVATE, path, {'id_token': session['idToken'], 'application_id':'unrelated'}, internal, expected=403)
        sessions.append((own['pid'], session['idToken']))
        persisted_devices.append((device, value['pid'], value['account_id'], other['id_token']))
        if expected_issuer:
            body = session['idToken'].split('.')[1]
            assert json.loads(base64.urlsafe_b64decode(body + '=' * (-len(body) % 4)))['iss'] == expected_issuer

    for optional in ('/shop', '/news', '/parental-controls'):
        send(NX, optional, expected=501)

    if restart:
        restart()
        account.ready(PRIVATE, '/healthz')
        for previous in sessions:
            account.call(PRIVATE, path, {'id_token': previous[1]}, internal, expected=404)
            account.call(PRIVATE, path, {'pid': previous[0]}, internal, expected=404)
        sessions = []
        for device, pid, owner, other_token in persisted_devices:
            send(NX, '/1.0.0/login', {**device, 'password': 'wrong'}, expected=400, form=True)
            send(NX, '/1.0.0/federation', {**device, 'idToken': other_token}, expected=403, form=True)
            login, _ = send(NX, '/1.0.0/login', device, form=True)
            value, _ = account.call(PRIVATE, path, {'id_token': login['idToken']}, internal)
            assert value['pid'] == pid and value['account_id'] == owner
            connected, _ = account.call(PRIVATE, path, {'pid': pid}, internal)
            assert connected == value
            membership = f"/v1/users/{login['user']['id']}/membership?" + urlencode({'application_id': APP})
            active, _ = send(NX, membership, headers={'Authorization': 'Bearer ' + login['accessToken']})
            assert active['membership']['active'] is True
            sessions.append((pid, login['idToken']))

    account.write_fixture(LAB / 'game-test.json', {'RC_IDENTITY_URL': PRIVATE, 'RC_IDENTITY_KEY': game_key,
        'RC_HOST_TOKEN': sessions[0][1], 'RC_GUEST_TOKEN': sessions[1][1],
        'RC_HOST_PID': sessions[0][0], 'RC_GUEST_PID': sessions[1][0]})
    result = {'owned_browser_approvals':2,'native_route_device_accounts':2,
        'valid_federation_then_login':2,'default_deny_before_federation':True,'wrong_owner_rejected':True,
        'membership_scoped_to_active_grant':True,'signed_identity_purpose_checked':True,'private_pid_revalidation_requires_live_login':True,
        'optional_services_explicitly_unsupported':True,'tls_verified':True,'retail_console_tested':False}
    if restart:
        result['restart'] = {'fresh_process':True,'retained_device_passwords':2,'same_pids_and_owners':2,
            'old_identity_tokens_denied':2,'wrong_password_and_owner_denied':True, 'membership_rechecked':True}
    if expected_issuer:
        result['issuer_different_from_bind_verified'] = True
    (receipt_path or ROOT / 'local-receipt.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Both own accounts passed native-shaped registration, federation, login and scoped membership.')


if __name__ == '__main__':
    main()
