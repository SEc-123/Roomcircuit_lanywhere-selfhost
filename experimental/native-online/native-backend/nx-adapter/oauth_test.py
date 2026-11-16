#!/usr/bin/env python3
"""Exercise the owned OAuth authorization and native federation path on loopback."""
import argparse
import base64
import hashlib
import http.cookies
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import tempfile
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlencode, urlparse
from urllib.request import HTTPRedirectHandler, HTTPSHandler, ProxyHandler, Request, build_opener

import smoke
from restart_test import local_port


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, help='Use a prebuilt executable for this host.')
    parser.add_argument('--broker-origin', default='https://127.0.0.1:39280')
    parser.add_argument('--grant-origin', default='http://127.0.0.1:39281')
    args = parser.parse_args()
    root = smoke.ROOT
    upstream = root.parent / '.runtime'
    browser = json.loads((upstream / 'browser-test.json').read_text())
    with tempfile.TemporaryDirectory(prefix='roomcircuit-nx-oauth-') as temporary:
        temp = Path(temporary)
        binary = args.binary.resolve(strict=True) if args.binary else temp / 'nx-adapter'
        if not args.binary:
            subprocess.run(['go', 'build', '-mod=readonly', '-o', str(binary), '.'], cwd=root, check=True)
        issuer_key = temp / 'issuer-key.pem'
        na_key = temp / 'na-issuer-key.pem'
        for key in (issuer_key, na_key):
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048',
                            '-out', str(key)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        state_key = temp / 'state-key'
        fd = os.open(state_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(secrets.token_hex(32))
        public, private = local_port(), local_port()
        while private == public:
            private = local_port()
        issuer = f'https://127.0.0.1:{public}'
        game_key = secrets.token_hex(32)
        client_id, redirect = 'roomcircuit-owned-fixture', 'roomcircuit://device-link'
        env = {**os.environ, 'NX_LAB_ISSUER': issuer, 'NX_LAB_LISTEN': f'127.0.0.1:{public}',
               'NX_LAB_INTERNAL': f'127.0.0.1:{private}', 'NX_LAB_GRANT_URL': args.grant_origin,
               'NX_LAB_GRANT_KEY': (upstream / 'game-key').read_text().strip(), 'NX_LAB_GAME_KEY': game_key,
               'NX_LAB_APPLICATION': smoke.APP, 'NX_LAB_SIGNING_KEY': str(issuer_key),
               'NX_LAB_CERT': str(upstream / 'server.pem'), 'NX_LAB_TLS_KEY': str(upstream / 'server-key.pem'),
               'NX_LAB_STATE': str(temp / 'devices.enc'), 'NX_LAB_STATE_KEY': str(state_key),
               'NX_LAB_OAUTH_REDIRECT': redirect, 'NX_LAB_OAUTH_CLIENT_ID': client_id,
               'NX_LAB_NA_ISSUER': issuer, 'NX_LAB_NA_SIGNING_KEY': str(na_key),
               'NX_LAB_BROWSER_CA': str(upstream / 'ca.pem'), 'NX_LAB_BROWSER_ORIGIN': args.broker_origin,
               'NX_LAB_ACCOUNT_URL': 'http://127.0.0.1:39170'}
        trust = ssl.create_default_context(cafile=str(upstream / 'ca.pem'))
        client = build_opener(ProxyHandler({}), HTTPSHandler(context=trust), NoRedirect())

        def fetch(path, form=None, headers=None, expected=200):
            payload = urlencode(form).encode() if form is not None else None
            req = Request(issuer + path, payload, {'Content-Type': 'application/x-www-form-urlencoded', **(headers or {})})
            try:
                response = client.open(req, timeout=10)
            except HTTPError as error:
                response = error
            with response:
                assert response.status == expected, f'{path.split("?")[0]} returned {response.status}, wanted {expected}'
                text = response.read().decode()
                return json.loads(text) if 'application/json' in response.headers.get('Content-Type', '') else text, response.headers

        root_path = '/connect/1.0.0'

        def b64url(raw):
            return base64.urlsafe_b64decode(raw + '=' * (-len(raw) % 4))

        def independently_verify(value, jwk, aud, key_path='/1.0.0/certificates'):
            raw_header, raw_claims, raw_signature = value.split('.')
            header, claims = json.loads(b64url(raw_header)), json.loads(b64url(raw_claims))
            assert header == {'alg':'RS256','typ':'JWT','kid':jwk['kid'],'jku':issuer+key_path}
            assert claims['iss']==issuer and claims['aud']==aud and claims['exp']>claims['iat']
            m, e, sig = (int.from_bytes(b64url(raw),'big') for raw in (jwk['n'],jwk['e'],raw_signature))
            size = (m.bit_length()+7)//8
            digest = hashlib.sha256((raw_header+'.'+raw_claims).encode()).digest()
            block = bytes.fromhex('3031300d060960864801650304020105000420')+digest
            assert pow(sig,e,m).to_bytes(size,'big')==b'\x00\x01'+b'\xff'*(size-len(block)-3)+b'\x00'+block
            return claims

        def browser_grant(credentials,jwk,client=client_id,callback=redirect):
            verifier = secrets.token_hex(32)
            hashed = hashlib.sha256(verifier.encode()).digest()
            challenge = base64.urlsafe_b64encode(hashed).decode().rstrip('=')
            state = secrets.token_hex(16)
            nonce = secrets.token_hex(16)
            query = {'client_id': client, 'redirect_uri': callback, 'response_type': 'session_token_code',
                     'session_token_code_challenge': challenge, 'session_token_code_challenge_method': 'S256', 'state':state,
                     'nonce':nonce,'scope':'openid user'}
            fetch(root_path+'/authorize?'+urlencode({**query,'redirect_uri':'https://unregistered.example.test'}), expected=400)
            page, headers = fetch(root_path+'/authorize?'+urlencode(query))
            form_token = re.search(r'name="form_token" value="([0-9a-f]+)"', page)[1]
            cookie = http.cookies.SimpleCookie(headers['Set-Cookie'])['rc_nx_link'].value
            post = {**credentials, 'form_token':form_token}
            fetch(root_path+'/authorize', post, {'Cookie':'rc_nx_link=wrong','Origin':issuer}, expected=403)
            _, jump = fetch(root_path+'/authorize', post, {'Cookie':'rc_nx_link='+cookie,'Origin':issuer}, expected=302)
            destination = urlparse(jump['Location'])
            original = urlparse(callback)
            assert destination.netloc == original.netloc and destination.scheme == original.scheme
            values = parse_qs(destination.query)
            assert values['state'] == [state] and set(values) == {'state','session_token_code'}
            code = values['session_token_code'][0]
            protected = {'client_id':client,'session_token_code':code,'session_token_code_verifier':verifier}
            fetch(root_path+'/api/token', {'client_id':client,'session_token':code}, expected=400)
            fetch(root_path+'/api/session_token', {**protected,'session_token_code_verifier':'A'*64}, expected=400)
            fetch(root_path+'/api/session_token', {**protected,'client_id':'other-client'}, expected=400)
            verified, _ = fetch(root_path+'/api/session_token', protected)
            assert verified['session_token'] == verified['code'] == code
            fetch(root_path+'/api/session_token', protected, expected=400)
            exchange = {'client_id':client,'session_token_code':code,'scope':'openid user'}
            tokens, _ = fetch(root_path+'/api/token',exchange)
            fetch(root_path+'/api/token', exchange, expected=400)
            assert tokens['id_token'] != tokens['access_token'] and tokens['expires_in'] > 600
            assert tokens['scope']==['openid','user'] and tokens['token_type']=='Bearer'
            id_token=independently_verify(tokens['id_token'],jwk,client)
            access_token=independently_verify(tokens['access_token'],jwk,client)
            refresh_token=independently_verify(tokens['refresh_token'],jwk,client)
            session_token=independently_verify(tokens['session_token'],jwk,client)
            assert all(c['sub']==id_token['sub'] for c in (access_token,refresh_token,session_token))
            assert 'typ' not in id_token and id_token['nonce']==nonce and access_token['typ']=='token'
            assert id_token['at_hash']==base64.urlsafe_b64encode(hashlib.sha256(tokens['access_token'].encode()).digest()[:16]).decode().rstrip('=')
            assert refresh_token['typ']=='refresh' and session_token['typ']=='session' and 'rc:sealed' in refresh_token
            fetch('/2.0.0/users/me',headers={'Authorization':'Bearer '+tokens['id_token']},expected=401)
            profile,_=fetch('/2.0.0/users/me',headers={'Authorization':'Bearer '+tokens['access_token']})
            assert profile['email']==credentials['email'] and profile['birthday']=='1994-01-01'
            assert profile['emailVerified'] and profile['summary']['nintendo']['hasMembership']
            assert re.fullmatch(r'[a-f0-9]{16}',profile['id']) and profile['nickname'] and profile['id']==id_token['sub']
            refresh={'client_id':client,'refresh_token':tokens['refresh_token']}
            rotated,_=fetch(root_path+'/api/token',refresh)
            assert rotated['refresh_token']!=tokens['refresh_token']
            assert independently_verify(rotated['refresh_token'],jwk,client)['sub']==profile['id']
            fetch(root_path+'/api/token',refresh,expected=401)
            return rotated['id_token'],id_token['sub'],rotated['refresh_token']

        process = subprocess.Popen([str(binary)], env=env)
        try:
            local = f'http://127.0.0.1:{private}'
            smoke.account.ready(local,'/healthz')
            certs,_=fetch('/1.0.0/certificates')
            jwk=certs['keys'][0]
            baas_id_path='/1.0.0/baas_certificates'
            baas_access_path='/1.0.0/internal_certificates'
            baas_id_keys,_=fetch(baas_id_path)
            baas_access_keys,_=fetch(baas_access_path)
            baas_id_jwk,baas_access_jwk=baas_id_keys['keys'][0],baas_access_keys['keys'][0]
            assert len({jwk['kid'],baas_id_jwk['kid'],baas_access_jwk['kid']})==3
            assert baas_id_jwk['n']==baas_access_jwk['n'] and baas_id_jwk['n']!=jwk['n']
            (host,host_na,_),(guest,guest_na,_)=(browser_grant(browser[name],jwk) for name in ('host','guest'))
            records = []
            latest_refresh = None
            for role, proof, other, native_id in (('host',host,guest,host_na),('guest',guest,host,guest_na)):
                own, _ = fetch('/1.0.0/users', {}, {'Authorization':'Bearer '+proof}, expected=201)
                credentials = own['deviceAccounts'][0]
                fetch('/1.0.0/federation', {**credentials,'idToken':other}, expected=403)
                result, _ = fetch('/1.0.0/federation', {**credentials,'idToken':proof})
                renewed, renewed_id, latest_refresh = browser_grant(browser[role],jwk)
                login, _ = fetch('/1.0.0/federation', {**credentials,'idToken':renewed})
                assert login['user']['id'] == result['user']['id'] == own['id']
                assert renewed_id==native_id and native_id!=own['id']
                assert set(login)=={'idToken','accessToken','expiresIn','tokenType','user'} and len(login['user'])==18
                assert login['user']['links']['nintendoAccount']['id']==native_id
                identity=independently_verify(login['idToken'],baas_id_jwk,smoke.APP,baas_id_path)
                access=independently_verify(login['accessToken'],baas_access_jwk,smoke.APP,baas_access_path)
                assert identity['sub']==access['sub']==login['user']['id']
                assert identity['bs:did']==access['bs:did']==json.loads(login['user']['etag'])
                assert identity['typ']=='id_token' and access['typ']=='token' and access['bs:grt']==2
                assert len(access['bs:sts'])==7 and identity['jti']!=access['jti'] and identity['exp']-identity['iat']==login['expiresIn']
                state, _ = smoke.account.call(local,'/internal/switch/identity',{'id_token':login['idToken']},
                                              {'X-Internal-Key':game_key})
                records.append(state['pid'])
            assert len(set(records)) == 2
            process.terminate()
            process.wait(timeout=6)
            process = subprocess.Popen([str(binary)], env=env)
            smoke.account.ready(local,'/healthz')
            refreshed,_=fetch(root_path+'/api/token',{'client_id':client_id,'refresh_token':latest_refresh})
            assert independently_verify(refreshed['id_token'],jwk,client_id)['sub']==guest_na
            logout={'client_id':client_id,'refresh_token':refreshed['refresh_token']}
            fetch(root_path+'/api/logout',logout,expected=204)
            fetch('/2.0.0/users/me',headers={'Authorization':'Bearer '+refreshed['access_token']},expected=404)
            fetch(root_path+'/api/token',logout,expected=404)
            (root / 'oauth-receipt.json').write_text(json.dumps({'verified_owned_accounts':2,'native_shaped_authorize_exchange':True,
                'registered_redirect_and_client_enforced':True,'pkce_and_single_use':True,'native_federation_after_exchange':True,
                'own_player_profile_requires_access_token':True,'oauth_refresh_rotates_and_rejects_replay':True,
                'independent_na_key_jwks_nonce_scope_and_access_hash_verified':True,'distinct_stable_na_and_baas_ids':True,
                'sealed_na_refresh_survives_adapter_restart':True,'na_logout_revokes_profile_and_renewal':True,
                'separate_baas_jwks_roles_binding_and_full_login_shape_verified':True,
                'reauthorization_same_owner_and_pid':True,'console_tested':False},indent=2)+'\n')
            print('Two owned OAuth link flows passed redirect, PKCE, one-time token exchange and federation.')
        finally:
            process.terminate()
            process.wait(timeout=6)


if __name__ == '__main__':
    main()
