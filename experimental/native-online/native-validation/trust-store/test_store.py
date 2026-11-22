#!/usr/bin/env python3
"""Bounded source parser tests, followed by extraction with independent hactool."""
import ctypes
import pathlib
import struct
import subprocess
import tempfile
import unittest

HERE=pathlib.Path(__file__).resolve().parent
WORK=tempfile.TemporaryDirectory(prefix='roomcircuit-trust-test-')
LIB=pathlib.Path(WORK.name)/'store.so'
subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(HERE/'store.c'),'-o',str(LIB)],check=True)
API=ctypes.CDLL(str(LIB))
API.rc_store_replace_expired_root.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p]
API.rc_store_romfs.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p]
FREE=ctypes.CDLL(None).free
FREE.argtypes=[ctypes.c_void_p]

def fixture():
    return bytearray(struct.pack('<II',0x546c7373,2)+struct.pack('<IIII',1,1,4,32)+struct.pack('<IIII',1033,1,5,36)+b'ROOT' +b'OLDCA')

def replace(s,ca=b'NEW-CA'):
    out=ctypes.c_void_p();size=ctypes.c_size_t()
    r=API.rc_store_replace_expired_root(bytes(s),len(s),ca,len(ca),ctypes.byref(out),ctypes.byref(size))
    result=ctypes.string_at(out,size.value) if out.value else None
    FREE(out)
    return r,result

class StoreTests(unittest.TestCase):
    def test_preserves_all_original_certificate_payloads(self):
        s=fixture();r,b=replace(s)
        self.assertEqual(r,0)
        self.assertEqual(b[:32],s[:32])
        self.assertEqual(b[40:len(s)],s[40:])
        self.assertEqual(b[52:],b'NEW-CA')
        self.assertEqual(struct.unpack_from('<II',b,32),(6,44))
    def test_rejects_malformed_resources(self):
        cases=[]
        for offset,value in [(0,0),(4,513),(16,9999),(20,0),(24,1),(28,2),(36,33),(36,0xffffffff)]:
            s=fixture();struct.pack_into('<I',s,offset,value);cases.append(s)
        cases.extend([fixture()[:7],fixture()[:-1],bytearray(b'\0')*(128*1024+1)])
        for s in cases:
            with self.subTest(data=bytes(s[:40])):
                self.assertEqual(replace(s),(-1,None))
        self.assertEqual(replace(fixture(),b''),(-1,None))
    def test_independent_romfs_extraction(self):
        r,b=replace(fixture());self.assertEqual(r,0)
        out=ctypes.c_void_p();size=ctypes.c_size_t()
        self.assertEqual(API.rc_store_romfs(b,len(b),ctypes.byref(out),ctypes.byref(size)),0)
        image=ctypes.string_at(out,size.value);FREE(out)
        p=pathlib.Path(WORK.name)/'romfs.bin';p.write_bytes(image)
        remote='/tmp/roomcircuit-trust-acceptance'
        subprocess.run(['docker','exec','switch-nodes-sdk-080','mkdir','-p',remote],check=True)
        subprocess.run(['docker','cp',str(p),'switch-nodes-sdk-080:'+remote+'/romfs.bin'],check=True,stdout=subprocess.DEVNULL)
        run=subprocess.run(['docker','exec','switch-nodes-sdk-080','/opt/devkitpro/tools/bin/hactool','-t','romfs','--romfsdir='+remote+'/out',remote+'/romfs.bin'],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        actual=subprocess.run(['docker','exec','switch-nodes-sdk-080','cat',remote+'/out/ssl_TrustedCerts.bdf'],capture_output=True,check=True).stdout
        self.assertEqual(actual,b)

if __name__=='__main__':
    unittest.main(verbosity=2)
