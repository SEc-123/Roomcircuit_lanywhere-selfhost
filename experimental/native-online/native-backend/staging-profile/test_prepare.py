#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import prepare

class PrepareTests(unittest.TestCase):
    def test_atomic_rename_never_replaces_existing_directory(self):
        with tempfile.TemporaryDirectory(prefix='rc-stage-rename-') as tmp:
            root=Path(tmp); source=root/'source'; target=root/'target'
            source.mkdir();target.mkdir();(target/'keep').write_text('preserved')
            with self.assertRaises(OSError):prepare.rename_new(source,target)
            self.assertTrue(source.is_dir())
            self.assertEqual((target/'keep').read_text(),'preserved')
            (target/'keep').unlink();target.rmdir()
            prepare.rename_new(source,target)
            self.assertTrue(target.is_dir())
            self.assertFalse(source.exists())

    def test_reject_unsafe_inputs_before_writing(self):
        with tempfile.TemporaryDirectory(prefix='rc-stage-test-') as tmp:
            for name in ('../unsafe','/absolute','with spaces'):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    prepare.prepare(tmp,name,tmp,'mario-party-superstars','/usr/bin')
            with self.assertRaises(ValueError):
                prepare.prepare(tmp,'stage',tmp,'mario-party-superstars','relative')
            with self.assertRaises(ValueError):
                prepare.prepare(tmp,'stage',tmp,'mario-party-superstars','/usr/bin',['/safe:relative'])
            self.assertEqual(list(Path(tmp).iterdir()),[])

    def test_real_offline_build_and_existing_state_preserved(self):
        source=os.environ.get('RC_NEX_SOURCE')
        if not source:self.skipTest('RC_NEX_SOURCE pinned source checkout is required for full profile build')
        with tempfile.TemporaryDirectory(prefix='rc-stage-test-') as tmp:
            stage=prepare.prepare(tmp,'stage',source,'mario-party-superstars','/opt/rc-pg/bin',['/opt/rc-pg/lib','/opt/rc-pg/pgsql'])
            data=json.loads((stage/'profile.json').read_text())
            self.assertEqual(data['application_id'],'01006fe013472000')
            self.assertFalse(data['device_enrollment_configured'])
            self.assertFalse(data['native_gameplay_verified'])
            self.assertEqual(len(data['binary_sha256']),7)
            for name,digest in data['binary_sha256'].items():
                self.assertEqual(hashlib.sha256((stage/'bin'/name).read_bytes()).hexdigest(),digest)
            for folder in ('env','keys'):
                for path in (stage/folder).iterdir():self.assertEqual(path.stat().st_mode&0o077,0)
            pg=(stage/'env/postgres.env').read_text()
            self.assertIn('LD_LIBRARY_PATH=/opt/rc-pg/lib:/opt/rc-pg/pgsql\n',pg)
            self.assertFalse((stage/'env/database.env').exists())
            self.assertEqual(len(json.loads((stage/'units/manifest.json').read_text())['units']),8)
            initial=hashlib.sha256((stage/'keys/device-state.key').read_bytes()).digest()
            with self.assertRaises(ValueError):prepare.prepare(tmp,'stage',source,'pico-park','/opt/rc-pg/bin')
            self.assertEqual(hashlib.sha256((stage/'keys/device-state.key').read_bytes()).digest(),initial)
            result=subprocess.run(['openssl','verify','-CAfile',str(stage/'tls/ca.pem'),str(stage/'tls/server.pem')],capture_output=True)
            self.assertEqual(result.returncode,0)

if __name__=='__main__':unittest.main(verbosity=2)
