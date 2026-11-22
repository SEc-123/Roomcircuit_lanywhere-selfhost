#!/usr/bin/env python3
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

from prepare import prepare

class Inputs(unittest.TestCase):
    def test_ca_address_and_matching_assets(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            for name,ca,days in [('first',True,1),('second',True,1),('leaf',False,1)]:
                command=['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days',str(days),
                    '-keyout',str(p/(name+'.key')),'-out',str(p/(name+'.pem')),
                    '-subj','/CN='+name,'-addext','basicConstraints=critical,CA:'+('TRUE' if ca else 'FALSE')]
                subprocess.run(command,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                subprocess.run(['openssl','x509','-in',str(p/(name+'.pem')),'-outform','DER',
                    '-out',str(p/(name+'.der'))],check=True)
            result=prepare(p/'first.der',p/'build','8.8.8.8')
            first=(p/'first.der').read_bytes()
            self.assertEqual(result,hashlib.sha256(first).hexdigest())
            self.assertEqual((p/'build/probe_ca.der').read_bytes(), first)
            self.assertEqual((p/'build/romfs/ca.der').read_bytes(), first)
            self.assertIn(result,(p/'build/rc_build_config.h').read_text())
            second=prepare(p/'second.der',p/'build','1.1.1.1')
            self.assertNotEqual(result,second)
            self.assertEqual((p/'build/probe_ca.der').read_bytes(),(p/'second.der').read_bytes())
            self.assertIn('1.1.1.1',(p/'build/rc_build_config.h').read_text())
            previous=(p/'build/rc_build_config.h').read_bytes()
            for address in ['not-an-ip','0.0.0.0','127.0.0.1','224.0.0.1','169.254.1.1']:
                with self.subTest(address=address), self.assertRaises(ValueError):
                    prepare(p/'first.der',p/'build',address)
            for name in ['leaf']:
                with self.subTest(ca=name), self.assertRaises(ValueError):
                    prepare(p/(name+'.der'),p/'build','1.1.1.1')
            with self.assertRaises(ValueError): prepare(Path(__file__).parent/'testdata/expired-ca.der',p/'build','1.1.1.1')
            (p/'invalid.der').write_bytes(b'not a certificate')
            with self.assertRaises(ValueError): prepare(p/'invalid.der',p/'build','1.1.1.1')
            self.assertEqual((p/'build/rc_build_config.h').read_bytes(),previous)

if __name__=='__main__':
    unittest.main(verbosity=2)
