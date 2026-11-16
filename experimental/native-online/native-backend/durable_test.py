#!/usr/bin/env python3
"""Verify own account grants across issuer restart, rotation and revocation."""
import datetime
import http.cookies
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import ssl
import subprocess
import tempfile
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPSHandler, ProxyHandler, Request, build_opener

import smoke


def ports(count):
    sockets = [socket.socket() for _ in range(count)]
    try:
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


def main():
    root = smoke.ROOT
    upstream = root.parent / 'account-lab'
    with tempfile.TemporaryDirectory(prefix='roomcircuit-durable-broker-') as temporary:
        temp = Path(temporary)
        account_dir = temp / 'account-lab'
        account_dir.mkdir()
        for name in ('compose.yaml', 'lab.sh'):
            shutil.copy2(upstream/name, account_dir/name)
        (account_dir/'.bin').mkdir()
        for name in ('account', 'identity'):
            shutil.copy2(upstream/'.bin'/name, account_dir/'.bin'/name)
        account_port, bridge_port, grpc_port, web_port, internal_port, nx_port, nx_internal_port = ports(7)
        project = 'roomcircuit-broker-proof-' + secrets.token_hex(4)
        compose = {**os.environ,'COMPOSE_PROJECT_NAME':project,'RC_ACCOUNT_PORT':str(account_port),
                   'RC_IDENTITY_PORT':str(bridge_port),'RC_ACCOUNT_GRPC_PORT':str(grpc_port)}
        smoke.account.ACCOUNT=f'http://127.0.0.1:{account_port}'
        smoke.account.IDENTITY=f'http://127.0.0.1:{bridge_port}'
        public=f'https://127.0.0.1:{web_port}'
        private=f'http://127.0.0.1:{internal_port}'
        nx_public=f'https://127.0.0.1:{nx_port}'
        nx_private=f'http://127.0.0.1:{nx_internal_port}'
        key=secrets.token_hex(32)
        game_auth={'X-Internal-Key':key}
        game_path='/internal/switch/identity'
        check_path='/internal/grants/check'
        app_a, app_b = '01006fe013472000', '01004ea00df70000'
        trust = ssl.create_default_context(cafile=str(root/'.runtime/ca.pem'))
        client=build_opener(ProxyHandler({}),HTTPSHandler(context=trust))
        binary=temp/'native-backend'
        subprocess.run(['go','build','-mod=readonly','-trimpath','-o',str(binary),'.'],cwd=root,check=True)
        nx_binary=temp/'nx-adapter'
        subprocess.run(['go','build','-mod=readonly','-trimpath','-o',str(nx_binary),'.'],cwd=root/'nx-adapter',check=True)
        state=temp/'approved-grants.json'
        proc=None
        nx_proc=None
        nx_key=secrets.token_hex(32)

        def call(path, data=None, headers=None, expected=200, form=False, origin=public):
            payload=(urlencode(data) if form else json.dumps(data)).encode() if data is not None else None
            mime='application/x-www-form-urlencoded' if form else 'application/json'
            req=Request(origin+path,payload,{'Content-Type':mime,**(headers or {})})
            try:
                result=client.open(req,timeout=10)
            except HTTPError as error:
                result=error
            with result:
                assert result.status==expected, f'{path.split("?")[0]} returned {result.status}, expected {expected}'
                body=result.read().decode()
                return (json.loads(body) if 'application/json' in result.headers.get('Content-Type','') else body), result.headers

        def grant(application, credentials):
            item,_=call('/v1/device/authorize',{'device_key':secrets.token_hex(32),'scope':'roomcircuit-game','application_id':application},expected=201)
            page,headers=call('/device?'+urlencode({'user_code':item['user_code']}))
            csrf=re.search(r'name="csrf_token" value="([a-f0-9]+)"',page)[1]
            cookie=http.cookies.SimpleCookie(headers['Set-Cookie'])['rc_device_form'].value
            call('/device',{'user_code':item['user_code'],'csrf_token':csrf,**credentials,'decision':'approve'},
                 {'Cookie':'rc_device_form='+cookie,'Origin':public},form=True)
            tokens,_=call('/v1/device/token',{'device_code':item['device_code']})
            return tokens

        def identity(tokens,application):
            value,_=smoke.account.call(private,game_path,{'id_token':tokens['id_token'],'application_id':application},game_auth)
            return value

        dotenv=account_dir/'.runtime/lab.env'
        try:
            subprocess.run(['sh','lab.sh','up'],cwd=account_dir,env=compose,check=True)
            smoke.account.ready(smoke.account.ACCOUNT,'/healthz')
            env_keys=dict(line.split('=',1) for line in dotenv.read_text().splitlines())
            env={**os.environ,'RC_DEVICE_ISSUER':public,'RC_DEVICE_LISTEN':f'127.0.0.1:{web_port}',
                 'RC_DEVICE_INTERNAL_LISTEN':f'127.0.0.1:{internal_port}','RC_ACCOUNT_URL':smoke.account.ACCOUNT,
                 'RC_LINKS_URL':smoke.account.IDENTITY,'RC_LINKS_KEY':env_keys['RC_IDENTITY_KEY'],
                 'RC_ACCOUNT_CORE_GRPC':f'127.0.0.1:{grpc_port}','RC_ACCOUNT_CORE_KEY':env_keys['RC_CORE_KEY'],
                 'RC_DEVICE_GAME_KEY':key,'RC_DEVICE_CERT':str(root/'.runtime/server.pem'),
                 'RC_DEVICE_TLS_KEY':str(root/'.runtime/server-key.pem'),'RC_DEVICE_SIGNING_KEY':str(root/'.runtime/issuer-key.pem'),
                 'RC_DEVICE_GRANT_STATE':str(state),'RC_DEVICE_APPLICATIONS':f'{app_a},{app_b}'}
            proc=subprocess.Popen([str(binary)],env=env)
            smoke.account.ready(private,'/healthz')
            players=(smoke.create_account('ProofHost','US')[0],smoke.create_account('ProofGuest','GB')[0])
            tickets=[grant(app_a,credentials) for credentials in players]
            originals=[identity(t,app_a) for t in tickets]
            assert originals[0]['account_id']!=originals[1]['account_id'] and originals[0]['pid']!=originals[1]['pid']
            assert all(ticket['refresh_token'].encode() not in state.read_bytes() for ticket in tickets)
            nx_store_key=temp/'nx-store-key'
            nx_store_key.write_text(secrets.token_hex(32))
            nx_store_key.chmod(0o600)
            nx_env={**os.environ,'NX_LAB_ISSUER':nx_public,'NX_LAB_LISTEN':f'127.0.0.1:{nx_port}',
                    'NX_LAB_INTERNAL':f'127.0.0.1:{nx_internal_port}','NX_LAB_GRANT_URL':private,
                    'NX_LAB_GRANT_KEY':key,'NX_LAB_GAME_KEY':nx_key,'NX_LAB_APPLICATION':app_a,
                    'NX_LAB_CERT':str(root/'.runtime/server.pem'),'NX_LAB_TLS_KEY':str(root/'.runtime/server-key.pem'),
                    'NX_LAB_SIGNING_KEY':str(root/'nx-adapter/.runtime/issuer-key.pem'),'NX_LAB_STATE':str(temp/'devices.enc'),
                    'NX_LAB_STATE_KEY':str(nx_store_key),'NX_LAB_OAUTH_REDIRECT':'','NX_LAB_DEVICE_SNI':''}
            nx_proc=subprocess.Popen([str(nx_binary)],env=nx_env)
            smoke.account.ready(nx_private,'/healthz')
            device_accounts=[]
            for profile,ticket in zip(originals,tickets):
                user,_=call('/1.0.0/users',{}, {'Authorization':'Bearer '+ticket['id_token']},201,origin=nx_public)
                device=user['deviceAccounts'][0]
                session,_=call('/1.0.0/federation',dict(device,idToken=ticket['id_token']),form=True,origin=nx_public)
                admitted,_=smoke.account.call(nx_private,game_path,{'id_token':session['idToken'],'application_id':app_a},
                                              {'X-Internal-Key':nx_key})
                assert admitted['pid']==profile['pid']
                device_accounts.append(device)
            first=proc.pid
            proc.terminate()
            proc.wait(timeout=6)
            proc=subprocess.Popen([str(binary)],env=env)
            assert proc.pid!=first
            smoke.account.ready(private,'/healthz')
            for profile,ticket in zip(originals,tickets):
                assert identity(ticket,app_a)==profile
                read,_=smoke.account.call(private,check_path,{'grant_id':profile['grant_id'],'application_id':app_b},game_auth)
                assert read['pid']==profile['pid'] and read['account_id']==profile['account_id'] and read['application_id']==app_b
            old=tickets[0]['refresh_token']
            call('/v1/device/refresh',{'refresh_token':old,'application_id':'0000000000000055'},expected=403)
            rotated,_=call('/v1/device/refresh',{'refresh_token':old,'application_id':app_b})
            assert rotated['refresh_token']!=old and identity(rotated,app_b)['pid']==originals[0]['pid']
            call('/v1/device/refresh',{'refresh_token':old,'application_id':app_b},expected=401)
            call('/v1/device/logout',{'refresh_token':rotated['refresh_token'],'application_id':app_b},expected=204)
            call('/v1/device/refresh',{'refresh_token':rotated['refresh_token'],'application_id':app_b},expected=401)
            smoke.account.call(private,check_path,{'grant_id':originals[0]['grant_id'],'application_id':app_b},game_auth,expected=404)
            smoke.account.call(nx_private,game_path,{'pid':originals[0]['pid'],'application_id':app_a},{'X-Internal-Key':nx_key},expected=403)
            # Expire only the second synthetic refresh record. The independent
            # device association stays usable until its owner revokes it.
            proc.terminate()
            proc.wait(timeout=6)
            saved=json.loads(state.read_text())
            saved['active'][originals[1]['grant_id']]['refresh_until']=(datetime.datetime.now(datetime.timezone.utc)-datetime.timedelta(seconds=1)).isoformat()
            state.write_text(json.dumps(saved))
            proc=subprocess.Popen([str(binary)],env=env)
            smoke.account.ready(private,'/healthz')
            call('/v1/device/refresh',{'refresh_token':tickets[1]['refresh_token'],'application_id':app_a},expected=401)
            still,_=smoke.account.call(private,check_path,{'grant_id':originals[1]['grant_id'],'application_id':app_a},game_auth)
            assert still['pid']==originals[1]['pid']
            nx_proc.terminate()
            nx_proc.wait(timeout=6)
            nx_proc=subprocess.Popen([str(nx_binary)],env=nx_env)
            smoke.account.ready(nx_private,'/healthz')
            call('/1.0.0/login',device_accounts[0],expected=404,form=True,origin=nx_public)
            resumed,_=call('/1.0.0/login',device_accounts[1],form=True,origin=nx_public)
            principal,_=smoke.account.call(nx_private,game_path,{'id_token':resumed['idToken'],'application_id':app_a},
                                          {'X-Internal-Key':nx_key})
            assert principal['pid']==originals[1]['pid']
            account_id=originals[1]['account_id']
            assert re.fullmatch(r'[0-9a-f-]{36}',account_id)
            # Explicit fixture-only state change on the disposable DB just
            # created above. The broker must re-read the real account state.
            subprocess.run(['docker','exec',project+'-db-1','psql','-U','account','-d','account','-Atc',
                            f"UPDATE accounts SET status='banned' WHERE id='{account_id}'"],check=True)
            smoke.account.call(private,check_path,{'grant_id':originals[1]['grant_id'],'application_id':app_a},game_auth,expected=403)
            smoke.account.call(nx_private,game_path,{'pid':originals[1]['pid'],'application_id':app_a},{'X-Internal-Key':nx_key},expected=403)
            (root/'durable-receipt.json').write_text(json.dumps({'two_verified_accounts':True,'broker_restart_preserved_live_grants':True,
                'independent_title_claims_same_player':True,'unknown_title_denied':True,'rotated_refresh_replay_denied':True,
                'logout_revokes_grant':True,'expired_refresh_denied_but_device_grant_remains':True,
                'adapter_restart_same_bound_pid_after_refresh_expiry':True,'live_game_admission_revoked_on_logout_or_disable':True,
                'disabled_account_denied':True,'plaintext_refresh_absent_from_store':True,'real_retail_console_tested':False},indent=2)+'\n')
            print('Two own account grants recovered after restart; refresh rotation, expiry, logout and account disable passed.')
        finally:
            if nx_proc is not None and nx_proc.poll() is None:
                nx_proc.terminate()
                nx_proc.wait(timeout=6)
            if proc is not None and proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=6)
            if dotenv.is_file():
                subprocess.run(['docker','compose','-p',project,'--env-file','.runtime/lab.env','down','--volumes'],cwd=account_dir,env=compose,check=True)


if __name__=='__main__':
    main()
