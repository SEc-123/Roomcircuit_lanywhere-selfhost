#!/usr/bin/env python3
"""Disposable Postgres and four real service processes; no external email or console access."""
import base64, hashlib, hmac, http.cookies, json, os, pathlib, re, secrets, socket, ssl, subprocess, tempfile, time, urllib.error, urllib.parse, urllib.request
HERE=pathlib.Path(__file__).resolve().parent
BACK=HERE.parent
ROOT=BACK.parent

def freeport():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
class NoRedirect(urllib.request.HTTPRedirectHandler):
 def redirect_request(self, req, fp, code, msg, headers, newurl):return None

def main():
 name='rc-username-e2e-'+secrets.token_hex(5);dbpass=secrets.token_hex(20);processes=[]
 def shell(*args, **kwargs):return subprocess.run(args,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=120,**kwargs)
 def start(name,env,logs):
  f=open(logs/(name+'.log'),'ab');p=subprocess.Popen([str(logs/name)],env={**os.environ,**env},stdout=f,stderr=f);processes.append((p,f));return p
 def ready(url):
  for i in range(100):
   try:
    with urllib.request.urlopen(url,context=ssl._create_unverified_context(),timeout=0.3):return
   except urllib.error.HTTPError:return
   except Exception:time.sleep(.1)
  raise RuntimeError('service startup timeout: '+url)
 def request(url,method='GET',form=None,headers=None,expected=200):
  data=urllib.parse.urlencode(form).encode() if form is not None else None
  req=urllib.request.Request(url,data=data,method=method,headers={'Content-Type':'application/x-www-form-urlencoded',**(headers or {})})
  try:r=client.open(req,timeout=12)
  except urllib.error.HTTPError as e:r=e
  with r:
   body=r.read();assert r.status==expected, f'{urllib.parse.urlsplit(url).path}: HTTP {r.status}, expected {expected}; body={body[:300]!r}'
   c=r.headers.get('Content-Type','');return (json.loads(body) if 'json' in c else body.decode()),r.headers
 try:
  with tempfile.TemporaryDirectory(prefix='rc-username-e2e-') as tmp:
   tmp=pathlib.Path(tmp)
   shell('docker','run','-d','--name',name,'-e','POSTGRES_PASSWORD='+dbpass,'-e','POSTGRES_DB=rc_username_test','-p','127.0.0.1::5432','postgres:16')
   for i in range(70):
    if subprocess.run(['docker','exec',name,'pg_isready','-U','postgres','-d','rc_username_test'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode==0:break
    time.sleep(.5)
   else:raise RuntimeError('PostgreSQL did not start')
   port=subprocess.check_output(['docker','port',name,'5432/tcp'],text=True,timeout=10).strip();dsn='postgres://postgres:'+dbpass+'@'+port+'/rc_username_test?sslmode=disable'
   env={**os.environ,'RC_USERNAME_TEST_DATABASE_URL':dsn}
   subprocess.run(['go','test','-race','-count=1','-run','TestRealPostgresUsernameLifecycle','-v'],cwd=HERE,env=env,check=True,timeout=120)
   # Separate DB test created rows: clear only this disposable database.
   shell('docker','exec',name,'psql','-U','postgres','-d','rc_username_test','-c','DROP SCHEMA public CASCADE; CREATE SCHEMA public;')
   for src,out in [(HERE,'core'),(ROOT/'account-lab/identity','identity'),(BACK,'broker'),(BACK/'nx-adapter','adapter')]:shell('go','build','-mod=readonly','-o',str(tmp/out),'.',cwd=src)
   shell('go','build','-mod=readonly','-o',str(tmp/'link-helper'),'./testdata/core-unlink',cwd=BACK)
   ports=set()
   while len(ports)<7:ports.add(freeport())
   core_http,core_grpc,ident_http,broker_pub,broker_private,adapter_pub,adapter_private=ports
   shared,links_key,game_key,adapter_game_key=[secrets.token_hex(32) for _ in range(4)]
   shell('openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','2','-keyout',str(tmp/'tls.key'),'-out',str(tmp/'tls.pem'),'-subj','/CN=127.0.0.1','-addext','subjectAltName=IP:127.0.0.1')
   for p in ('broker-key','adapter-key','na-key'):shell('openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(tmp/(p+'.pem')))
   (tmp/'device-state-key').write_text(secrets.token_hex(32));(tmp/'device-state-key').chmod(0o600)
   coreurl=f'http://127.0.0.1:{core_http}';brokerurl=f'https://127.0.0.1:{broker_pub}';adapterurl=f'https://127.0.0.1:{adapter_pub}'
   core_env={'RC_USERNAME_DATABASE_URL':dsn,'RC_USERNAME_CORE_KEY':shared,'RC_USERNAME_HTTP':f'127.0.0.1:{core_http}','RC_USERNAME_GRPC':f'127.0.0.1:{core_grpc}'}
   core_process=start('core',core_env,tmp);ready(coreurl+'/healthz')
   start('identity',{'RC_CORE_HTTP':coreurl,'RC_CORE_GRPC':f'127.0.0.1:{core_grpc}','RC_CORE_KEY':shared,'RC_IDENTITY_KEY':links_key,'RC_IDENTITY_LISTEN':f'127.0.0.1:{ident_http}','RC_ACCOUNT_REALM':'username'},tmp);ready(f'http://127.0.0.1:{ident_http}/healthz')
   start('broker',{'RC_DEVICE_ISSUER':brokerurl,'RC_ACCOUNT_URL':coreurl,'RC_LINKS_URL':f'http://127.0.0.1:{ident_http}','RC_LINKS_KEY':links_key,'RC_DEVICE_GAME_KEY':game_key,'RC_DEVICE_SIGNING_KEY':str(tmp/'broker-key.pem'),'RC_DEVICE_CERT':str(tmp/'tls.pem'),'RC_DEVICE_TLS_KEY':str(tmp/'tls.key'),'RC_DEVICE_LISTEN':f'127.0.0.1:{broker_pub}','RC_DEVICE_INTERNAL_LISTEN':f'127.0.0.1:{broker_private}','RC_ACCOUNT_CORE_GRPC':f'127.0.0.1:{core_grpc}','RC_ACCOUNT_CORE_KEY':shared,'RC_ACCOUNT_REALM':'username','RC_DEVICE_GRANT_STATE':str(tmp/'grants.json'),'RC_DEVICE_APPLICATIONS':'0000000000001234'},tmp);ready(brokerurl+'/healthz')
   start('adapter',{'NX_LAB_ISSUER':adapterurl,'NX_LAB_LISTEN':f'127.0.0.1:{adapter_pub}','NX_LAB_INTERNAL':f'127.0.0.1:{adapter_private}','NX_LAB_GRANT_URL':f'http://127.0.0.1:{broker_private}','NX_LAB_GRANT_KEY':game_key,'NX_LAB_GAME_KEY':adapter_game_key,'NX_LAB_APPLICATION':'0000000000001234','NX_LAB_SIGNING_KEY':str(tmp/'adapter-key.pem'),'NX_LAB_CERT':str(tmp/'tls.pem'),'NX_LAB_TLS_KEY':str(tmp/'tls.key'),'NX_LAB_STATE':str(tmp/'devices.enc'),'NX_LAB_STATE_KEY':str(tmp/'device-state-key'),'NX_LAB_OAUTH_REDIRECT':'roomcircuit://device-link','NX_LAB_OAUTH_CLIENT_ID':'roomcircuit-owned-fixture','NX_LAB_NA_ISSUER':adapterurl,'NX_LAB_NA_SIGNING_KEY':str(tmp/'na-key.pem'),'NX_LAB_BROWSER_CA':str(tmp/'tls.pem'),'NX_LAB_BROWSER_ORIGIN':brokerurl,'NX_LAB_ACCOUNT_URL':coreurl,'NX_LAB_ACCOUNT_REALM':'username'},tmp);ready(f'http://127.0.0.1:{adapter_private}/healthz')
   client=urllib.request.build_opener(urllib.request.ProxyHandler({}),urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=str(tmp/'tls.pem'))),NoRedirect())
   page,heads=request(brokerurl+'/register');cookie=http.cookies.SimpleCookie(heads['Set-Cookie'])['rc_register'].value;csrf=re.search('name="csrf" value="([0-9a-f]+)"',page)[1]
   form={'csrf':csrf,'username':'tester01','password':'correct-password-1'};headers={'Cookie':'rc_register='+cookie,'Origin':brokerurl,'X-Internal-Key':game_key}
   request(brokerurl+'/register','POST',form,headers);request(brokerurl+'/register','POST',form,headers,409)
   verifier=secrets.token_hex(32);challenge=base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
   oauth={'client_id':'roomcircuit-owned-fixture','redirect_uri':'roomcircuit://device-link','response_type':'session_token_code','session_token_code_challenge':challenge,'session_token_code_challenge_method':'S256','state':'state123','scope':'openid user'}
   page,heads=request(adapterurl+'/connect/1.0.0/authorize?'+urllib.parse.urlencode(oauth));token=re.search('name="form_token" value="([0-9a-f]+)"',page)[1];cookie=http.cookies.SimpleCookie(heads['Set-Cookie'])['rc_nx_link'].value
   _,heads=request(adapterurl+'/connect/1.0.0/authorize','POST',{'form_token':token,'username':'tester01','password':'correct-password-1'},{'Cookie':'rc_nx_link='+cookie,'Origin':adapterurl},302)
   code=urllib.parse.parse_qs(urllib.parse.urlsplit(heads['Location']).query)['session_token_code'][0]
   request(adapterurl+'/connect/1.0.0/api/session_token','POST',{'client_id':'roomcircuit-owned-fixture','session_token_code':code,'session_token_code_verifier':verifier})
   tokens,_=request(adapterurl+'/connect/1.0.0/api/token','POST',{'client_id':'roomcircuit-owned-fixture','session_token_code':code,'scope':'openid user'})
   profile,_=request(adapterurl+'/2.0.0/users/me',headers={'Authorization':'Bearer '+tokens['access_token']});assert profile['email']=='' and profile['emailVerified'] is False and profile['birthday']=='' and profile['country']==''
   user,_=request(adapterurl+'/1.0.0/users','POST',{}, {'Authorization':'Bearer '+tokens['id_token']},201);credentials=user['deviceAccounts'][0]
   joined,_=request(adapterurl+'/1.0.0/federation','POST',{**credentials,'idToken':tokens['id_token']});assert joined['user']['id']==user['id']
   def identity(value, expected=200):
    req=urllib.request.Request(f'http://127.0.0.1:{adapter_private}/internal/switch/identity',json.dumps({'id_token':value}).encode(),{'Content-Type':'application/json','X-Internal-Key':adapter_game_key},method='POST')
    try:r=client.open(req,timeout=10)
    except urllib.error.HTTPError as e:r=e
    with r:
     body=r.read();assert r.status==expected,f'game identity status {r.status}, expected {expected}'
     return json.loads(body)
   identity('invalid-token',401)
   who=identity(joined['idToken']);assert who['account_type']=='username' and who['pid'] and len(who['account_id'])==36
   # Restart only the disposable core; persisted sessions and account/link state remain valid.
   core_process.terminate();core_process.wait(timeout=5);core_process=start('core',core_env,tmp);ready(coreurl+'/healthz')
   assert identity(joined['idToken'])['pid']==who['pid']
   shell('docker','exec',name,'psql','-U','postgres','-d','rc_username_test','-c',"UPDATE rc_user_account SET state='disabled' WHERE username='tester01'")
   identity(joined['idToken'],403)
   shell('docker','exec',name,'psql','-U','postgres','-d','rc_username_test','-c',"UPDATE rc_user_account SET state='active' WHERE username='tester01'")
   # Unlink and reactivate the identical link. Its old grant must not revive.
   material=('roomcircuit-oauth-device-v1\0'+adapterurl+'\0'+who['account_id']).encode()
   device=hmac.new(adapter_game_key.encode(),material,hashlib.sha256).digest()
   subject=hashlib.sha256(b'roomcircuit-lab:'+device).hexdigest()
   env={**os.environ,'RC_TEST_CORE_ADDRESS':f'127.0.0.1:{core_grpc}','RC_TEST_CORE_KEY':shared}
   action=json.dumps({'account':who['account_id'],'subject':subject})
   bad={**env,'RC_TEST_CORE_KEY':'wrong-key-'+'0'*54}
   rejected=subprocess.run([str(tmp/'link-helper')],input=action,text=True,env=bad,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=20)
   assert rejected.returncode!=0, 'gRPC accepted an invalid internal key'
   subprocess.run([str(tmp/'link-helper')],input=action,text=True,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True,timeout=20)
   identity(joined['idToken'],403)
   print('PASS real Postgres -> username registration -> password login -> device approval -> OAuth -> email-free profile -> BAAS federation -> game identity -> restart -> disabled account and unlink/relink rejection')
 finally:
  for p,f in reversed(processes):
   if p.poll() is None:p.terminate()
  for p,f in reversed(processes):
   try:p.wait(timeout=5)
   except subprocess.TimeoutExpired:p.kill();p.wait()
   f.close()
  subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=20)
if __name__=='__main__':main()
