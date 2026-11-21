#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
import unittest
from prepare_endpoint import create

class Assets(unittest.TestCase):
    def test_owned_ca_key_san_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            for name in ['own','other','restricted']:
                extra = ['-addext','nameConstraints=critical,permitted;DNS:roomcircuit.native'] if name == 'restricted' else []
                subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
                    '-keyout',str(p/(name+'.key')),'-out',str(p/(name+'.pem')),
                    '-subj','/CN='+name,'-addext','basicConstraints=critical,CA:TRUE']+extra,
                    check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            with self.assertRaises(ValueError):
                create(p/'own.pem',p/'other.key','1.1.1.1',['accounts.nintendo.com'],p/'bad')
            self.assertFalse((p/'bad').exists())
            with self.assertRaises(ValueError):
                create(p/'restricted.pem',p/'restricted.key','1.1.1.1',['accounts.nintendo.com'],p/'bad')
            self.assertFalse((p/'bad').exists())
            dest=create(p/'own.pem',p/'own.key','1.1.1.1',['accounts.nintendo.com'],p/'out')
            self.assertEqual((dest/'address.txt').read_text(),'1.1.1.1\n')
            cert=subprocess.run(['openssl','x509','-in',str(dest/'server.pem'),'-noout','-text'],
                                check=True,capture_output=True,text=True).stdout
            self.assertIn('DNS:accounts.nintendo.com',cert)
            self.assertIn('1.1.1.1',cert)
            self.assertEqual((dest/'server.key').stat().st_mode & 0o777,0o600)
            before=(dest/'server.pem').read_bytes()
            with self.assertRaises(ValueError):
                create(p/'own.pem',p/'own.key','1.1.1.1',['accounts.nintendo.com'],dest)
            self.assertEqual((dest/'server.pem').read_bytes(),before)
            for names in [[],['wrong.example'],['accounts.nintendo.com','accounts.nintendo.com']]:
                with self.assertRaises(ValueError):create(p/'own.pem',p/'own.key','1.1.1.1',names,p/'bad')

if __name__=='__main__':unittest.main(verbosity=2)
