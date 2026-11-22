#!/usr/bin/env python3
import configparser
import json
import pathlib
import tempfile
import unittest
from unittest import mock

import render

class UnitTests(unittest.TestCase):
    def test_both_titles_loopback_and_readback(self):
        for title,app in render.TITLES.items():
            with self.subTest(title=title), tempfile.TemporaryDirectory() as tmp:
                root=pathlib.Path(tmp).resolve(); out=root/'units'; stage=str(root/'stage-path')
                manifest=render.render(stage,title,out)
                self.assertEqual(manifest['title_id'],app)
                self.assertEqual(manifest['total_memory_max_mib'],360)
                self.assertEqual(len(manifest['units']),8)
                for role in render.PARENTS:
                    name=render.PREFIX+role+'.service'; text=(out/name).read_text()
                    config=configparser.ConfigParser(interpolation=None,strict=False);config.read_string(text)
                    service=config['Service']; self.assertEqual(service['User'],render.SERVICE_USER)
                    self.assertEqual(service['Group'],render.SERVICE_USER)
                    self.assertEqual(service['NoNewPrivileges'],'yes')
                    self.assertEqual(service['ProtectSystem'],'strict')
                    self.assertEqual(service['IPAddressDeny'],'any')
                    self.assertEqual(service['IPAddressAllow'],'localhost')
                    self.assertEqual(service['MemoryMax'],str(render.CAPS_MIB[role])+'M')
                    self.assertIn('"'+stage+'/bin/'+render.BIN[role]+'"',service['ExecStart'])
                    self.assertNotIn('0.0.0.0',service['ExecStart'])
                    self.assertNotIn('\nExecStartPre=',text)
                    self.assertIn('EnvironmentFile='+stage+'/env/',text)
                    if role=='core':self.assertIn(stage+'/env/database.env',text)
                    if role in ('adapter','broker'):self.assertIn(app,service['ExecStart'])
                    if role=='nex':
                        self.assertIn('"-only='+title+'"',service['ExecStart'])
                        self.assertIn('"-bind-address=127.0.0.1"',service['ExecStart'])
                        self.assertIn('"-station-address=127.0.0.1"',service['ExecStart'])
                saved=json.loads((out/'manifest.json').read_text());self.assertEqual(saved,manifest)
                stack=(out/(render.PREFIX+'stack.target')).read_text()
                for role in render.PARENTS:self.assertIn(render.PREFIX+role+'.service',stack)
                # Re-rendering must not replace any existing unit.
                with self.assertRaises(FileExistsError):render.render(stage,title,out)
                self.assertEqual(json.loads((out/'manifest.json').read_text()),manifest)

    def test_rejects_unsafe_stage_and_unsupported_title(self):
        for value in ['/', 'relative', '/opt/../etc', '/opt/a%h', '/opt/a\nExecStart=/bin/false', '/opt/a"b', '/opt/a\\b', '/opt/$HOME', '/opt/a@b', '/opt/stage path']:
            with self.subTest(stage=value):
                with self.assertRaises(ValueError):render.check_stage(value)
        with tempfile.TemporaryDirectory() as tmp:
            out=pathlib.Path(tmp)/'units'
            with self.assertRaises(ValueError):render.render('/var/lib/rc-test','unknown',out)
            self.assertFalse(out.exists())

    def test_failed_render_cleans_only_new_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp); existing=root/'preserved.txt'; existing.write_text('keep')
            out=root/'units'; original=render.unit
            def broken(stage,title,role):
                if role=='broker': raise OSError('simulated write failure')
                return original(stage,title,role)
            with mock.patch.object(render,'unit',side_effect=broken):
                with self.assertRaises(OSError):render.render('/var/lib/rc-test','pico-park',out)
            self.assertFalse(out.exists()); self.assertEqual(existing.read_text(),'keep')

if __name__ == '__main__':unittest.main(verbosity=2)
