#!/usr/bin/env python3
"""Two real Go processes, synthetic certificates, loopback only."""
import base64
import hashlib
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
import time
import unittest
import urllib.parse

import create_local_profile as profile
import generate_test_ca
import prepare_endpoint
import sys
HERE = Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent/'preflight'))
from check import probe, ALIASES

ROOT=HERE.parents[1]
ACCOUNT='accounts.nintendo.com'
DAUTH='dauth-lp1.ndas.srv.nintendo.net'


def run(*args, data=None):
    result=subprocess.run(args,input=data,capture_output=True,timeout=90)
    if result.returncode:
        raise RuntimeError('command failed: '+args[0]+' '+result.stderr.decode(errors='replace')[-800:])
    return result.stdout


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]


def wait_started(process, value):
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        if process.poll() is not None:
            raise RuntimeError('process exited before listening: '+process.stderr.read().decode(errors='replace')[-1500:])
        try:
            with socket.create_connection(('127.0.0.1',value),timeout=.2):return
        except OSError:time.sleep(.05)
    raise RuntimeError('loopback process did not start')


def request(ca, value, host, path, method='GET', body=b'', cert=None):
    ctx=ssl.create_default_context(cafile=str(ca))
    ctx.verify_flags |= ssl.VERIFY_X509_STRICT
    if cert:ctx.load_cert_chain(str(cert[0]),str(cert[1]))
    with socket.create_connection(('127.0.0.1',value),timeout=3) as raw:
        with ctx.wrap_socket(raw,server_hostname=host) as s:
            s.settimeout(3)
            wire=(method+' '+path+' HTTP/1.1\r\nHost: '+host+'\r\nConnection: close\r\n'
                  'Content-Type: application/x-www-form-urlencoded\r\nContent-Length: '+str(len(body))+'\r\n\r\n').encode()+body
            s.sendall(wire);data=b''
            while len(data)<131072:
                chunk=s.recv(8192)
                if not chunk:break
                data+=chunk
    first,_,tail=data.partition(b'\r\n')
    return int(first.split()[1]),tail


class RealLoopback(unittest.TestCase):
    def test_restricted_ca_sdk_routes_and_enrollment(self):
        with tempfile.TemporaryDirectory(prefix='roomcircuit-local-profile-') as d:
            root=Path(d); names=list(profile.SDK_ROUTES)
            ca=generate_test_ca.create(names,root/'ca')
            assets=prepare_endpoint.create(ca/'ca.pem',ca/'ca.key','8.8.8.8',names,root/'endpoint')
            # The SAN and trust chain are from the same generated CA. An unrelated
            # supported alias must not be signable by this constrained CA.
            with self.assertRaises(ValueError):
                prepare_endpoint.create(ca/'ca.pem',ca/'ca.key','8.8.8.8',
                    [next(iter(profile.UNSUPPORTED))],root/'bad-endpoint')
            first, second, third = port(), port(), port()
            while len({first,second,third})<3:first,second,third=port(),port(),port()
            cfg=profile.create(assets,root/'profile',first,second,third)
            original=(assets/'ca.der').read_bytes()
            (assets/'ca.der').write_bytes(b'not the configured CA')
            with self.assertRaises(ValueError):
                profile.create(assets,root/'bad-profile',first,second,third)
            self.assertFalse((root/'bad-profile').exists())
            (assets/'ca.der').write_bytes(original)
            address=(assets/'address.txt').read_text()
            (assets/'address.txt').write_text('1.1.1.1\n')
            with self.assertRaises(ValueError):
                profile.create(assets,root/'bad-profile',first,second,third)
            self.assertFalse((root/'bad-profile').exists())
            (assets/'address.txt').write_text(address)
            with self.assertRaises(ValueError):
                profile.create(assets,root/'bad-profile',first,first,third)
            ingress=json.loads((cfg/'ingress.json').read_text())
            self.assertEqual(set(ingress['routes']),set(names))
            self.assertNotIn('no_sni',ingress)
            self.assertFalse(json.loads((cfg/'status.json').read_text())['device_enrollment_configured'])
            # Both binaries are built from the actual source; no existing service is used.
            adapter,router=root/'adapter',root/'ingress'
            supplied_adapter, supplied_router = os.getenv('RC_TEST_ADAPTER'), os.getenv('RC_TEST_INGRESS')
            if bool(supplied_adapter) != bool(supplied_router):
                self.fail('both prebuilt test binaries must be supplied together')
            if supplied_adapter:
                shutil.copyfile(supplied_adapter, adapter)
                shutil.copyfile(supplied_router, router)
                adapter.chmod(0o700)
                router.chmod(0o700)
            else:
                run('go','-C',str(ROOT/'native-backend/nx-adapter'),'build','-o',str(adapter),'.')
                run('go','-C',str(ROOT/'native-backend/ingress'),'build','-o',str(router),'.')
            for n in ('baas','na','dauth'):
                run('openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(root/(n+'.key')))
                (root/(n+'.key')).chmod(0o600)
            for n in ('device','other'):
                run('openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-keyout',str(root/(n+'.key')),
                    '-out',str(root/(n+'.pem')),'-subj','/CN=synthetic-'+n,
                    '-addext','keyUsage=critical,digitalSignature','-addext','extendedKeyUsage=clientAuth')
            pub=run('openssl','x509','-in',str(root/'device.pem'),'-pubkey','-noout')
            der=run('openssl','pkey','-pubin','-outform','DER',data=pub)
            pin=hashlib.sha256(der).hexdigest()
            (root/'state.key').write_text(secrets.token_hex(32));(root/'state.key').chmod(0o600)
            env={**os.environ, **json.loads((cfg/'adapter-env.template.json').read_text()),
                'NX_LAB_APPLICATION':'0000000000001234',
                'NX_LAB_SIGNING_KEY':str(root/'baas.key'),'NX_LAB_NA_SIGNING_KEY':str(root/'na.key'),
                'NX_LAB_DAUTH_SIGNING_KEY':str(root/'dauth.key'),
                'NX_LAB_STATE':str(root/'devices.enc'),'NX_LAB_STATE_KEY':str(root/'state.key'),
                'NX_LAB_BROWSER_CA':str(ca/'ca.pem'),'NX_LAB_BROWSER_ORIGIN':'https://127.0.0.1:39198',
                'NX_LAB_ACCOUNT_URL':'http://127.0.0.1:39197','NX_LAB_GRANT_URL':'http://127.0.0.1:39196',
                'NX_LAB_GRANT_KEY':secrets.token_hex(32),'NX_LAB_GAME_KEY':secrets.token_hex(32)}
            denied=subprocess.run([str(adapter)],env=env,capture_output=True,timeout=5)
            self.assertNotEqual(denied.returncode,0,'empty enrolled device list must fail closed')
            env['NX_LAB_ENROLLED_SPKI']=pin
            processes=[]
            try:
                a=subprocess.Popen([str(adapter)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE);processes.append(a)
                wait_started(a,second)
                r=subprocess.Popen([str(router),'-config',str(cfg/'ingress.json')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE);processes.append(r)
                wait_started(r,first)
                good=probe('127.0.0.1',first,names,str(ca/'ca.pem'),3)
                self.assertTrue(good['all_tls_verified'],good)
                foreign_ca=generate_test_ca.create(names,root/'foreign-ca')
                foreign=probe('127.0.0.1',first,[ACCOUNT],str(foreign_ca/'ca.pem'),3)
                self.assertFalse(foreign['all_tls_verified'],foreign)
                self.assertEqual(foreign['checks'][0]['stage'],'certificate',foreign)
                for host in profile.UNSUPPORTED:
                    result=probe('127.0.0.1',first,[host],str(ca/'ca.pem'),3)
                    self.assertFalse(result['all_tls_verified'],result)
                    self.assertEqual(result['checks'][0]['stage'],'connection_or_handshake',result)
                status,_=request(ca/'ca.pem',first,DAUTH,'/v7/challenge','POST',b'key_generation=19')
                self.assertEqual(status,401)
                status,_=request(ca/'ca.pem',first,DAUTH,'/v7/challenge','POST',b'key_generation=19',(root/'other.pem',root/'other.key'))
                self.assertEqual(status,401)
                status,_=request(ca/'ca.pem',first,DAUTH,'/v7/challenge','POST',b'key_generation=19',(root/'device.pem',root/'device.key'))
                self.assertEqual(status,200)
                verifier='a'*43
                challenge=base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
                q=urllib.parse.urlencode({'client_id':'6ffd70c434d303c8','redirect_uri':'nintendo://account.nx.sys',
                    'response_type':'session_token_code','scope':'nx','state':'local-only','session_token_code_challenge':challenge,
                    'session_token_code_challenge_method':'S256'})
                status,body=request(ca/'ca.pem',first,ACCOUNT,'/connect/1.0.0/authorize?'+q)
                self.assertEqual(status,200)
                self.assertIn(b'name="username"',body)
                self.assertNotIn(b'name="email"',body)
                # TLS v1.2 trace verifies which actual host requested a client cert.
                def tls_trace(host):
                    request_bytes=('GET /2.0.0/agreements HTTP/1.1\r\nHost: '+host+'\r\nConnection: close\r\n\r\n').encode()
                    return subprocess.run(['openssl','s_client','-tls1_2','-connect',f'127.0.0.1:{first}',
                        '-servername',host,'-CAfile',str(ca/'ca.pem'),'-cert',str(root/'device.pem'),
                        '-key',str(root/'device.key'),'-msg','-quiet'],input=request_bytes,capture_output=True,timeout=5)
                account_trace=tls_trace(ACCOUNT)
                device_trace=tls_trace(DAUTH)
                self.assertIn(b'HTTP/1.1 200',account_trace.stdout)
                self.assertIn(b'HTTP/1.1 200',device_trace.stdout)
                self.assertNotIn(b'CertificateRequest',account_trace.stdout)
                self.assertIn(b'CertificateRequest',device_trace.stdout)
            finally:
                for p in reversed(processes):
                    p.terminate()
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:p.kill();p.wait()
                    if p.stderr:p.stderr.close()

if __name__=='__main__':unittest.main(verbosity=2)
