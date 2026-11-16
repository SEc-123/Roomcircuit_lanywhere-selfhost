#!/usr/bin/env python3
"""Verify real selected-node persistence against a disposable filesystem."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
header = r'''
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
using u8=uint8_t;using u32=uint32_t;using s64=int64_t;using Result=int;
#define R_SUCCEEDED(r) ((r)==0)
#define R_FAILED(r) ((r)!=0)
namespace ams::crypto { inline void GenerateSha256(void*out,size_t n,const void*in,size_t sz){auto*p=(const u8*)in;u32 x=5381;for(size_t i=0;i<sz;i++)x=(x*33)^p[i];for(size_t i=0;i<n;i++)((u8*)out)[i]=(x>>(8*(i%4)))^i;} }
namespace ams::fs {
using FileHandle=FILE*;constexpr int OpenMode_Read=1,OpenMode_Write=2;
struct WriteOption { static constexpr int Flush=1; }; struct ResultPathNotFound { static bool Includes(int r){return r==2;} };
inline bool fail_write=false, fail_open=false;
inline Result OpenFile(FileHandle*f,const char*p,int mode){if(fail_open)return 1;*f=fopen(p,mode==1?"rb":"r+b");return *f?0:2;}
inline void CloseFile(FileHandle f){fclose(f);} inline Result GetFileSize(s64*n,FileHandle f){fseek(f,0,SEEK_END);*n=ftell(f);return *n<0?1:0;}
inline Result ReadFile(size_t*n,FileHandle f,s64 at,void*b,size_t size){fseek(f,at,SEEK_SET);*n=fread(b,1,size,f);return *n==size?0:1;}
inline Result WriteFile(FileHandle f,s64 at,const void*b,size_t n,int){if(fail_write)return 1;fseek(f,at,SEEK_SET);return fwrite(b,1,n,f)==n&&fflush(f)==0?0:1;}
inline Result DeleteFile(const char*p){std::error_code e;bool ok=std::filesystem::remove(p,e);return e?1:ok?0:2;}
inline Result CreateDirectory(const char*p){std::error_code e;std::filesystem::create_directories(p,e);return e?1:0;}
inline Result CreateFile(const char*p,s64){FILE*f=fopen(p,"wb");if(!f)return 1;fclose(f);return 0;}
inline Result RenameFile(const char*a,const char*b){std::error_code e;std::filesystem::rename(a,b,e);return e?1:0;}
inline Result HasFile(bool*found,const char*p){std::error_code e;*found=std::filesystem::exists(p,e);return e?1:0;}
}
'''
harness = r'''
#include "node_preferences.hpp"
#include "storage_migration.hpp"
#include <stratosphere.hpp>
#include <cassert>
#include <cstring>
namespace ams::mitm::storage { unsigned generation=0; bool StorageUseCurrent(){return generation==3;} unsigned StorageGeneration(){ return generation; } }
using namespace ams::mitm::lanywhere;
int main(){
 NodeDynamicEntry one{},two{},result{};one.id=2;one.port=11451;strcpy(one.host,"node.test");strcpy(one.name,"Node");
 two=one;two.id=3;strcpy(two.host,"second.test");
 assert(ReadNodePreference(&result)==NodePreferenceRead::Missing);
 assert(SaveNodePreference(one));assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==2);
 ams::fs::fail_write=true;assert(!SaveNodePreference(two));ams::fs::fail_write=false;
 assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==2);
 assert(SaveNodePreference(two));assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==3);
 {FILE*f=fopen("sdmc:/lanywhere/selected-node.bin","wb");assert(f);fputs("corrupt",f);fclose(f);}
 assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==2);
 {FILE*f=fopen("sdmc:/lanywhere/selected-node.bin.previous","wb");assert(f);fputs("bad",f);fclose(f);}
 assert(ReadNodePreference(&result)==NodePreferenceRead::Invalid);
 assert(SaveNodePreference(two));assert(std::filesystem::exists("sdmc:/lanywhere/selected-node.bin.invalid"));
 assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==3);
 ams::mitm::storage::generation=1;
 assert(ReadNodePreference(&result)==NodePreferenceRead::Missing);
 assert(SaveNodePreference(two));assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==3);
 ams::mitm::storage::generation=3;
 assert(ReadNodePreference(&result)==NodePreferenceRead::Missing);
 assert(SaveNodePreference(one));assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==2);
 ams::mitm::storage::generation=1;assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==3);
 ams::mitm::storage::generation=3;assert(ReadNodePreference(&result)==NodePreferenceRead::Valid && result.id==2);
 ams::fs::fail_open=true;assert(ReadNodePreference(&result)==NodePreferenceRead::Invalid);
 puts("PASS persisted node restart, interruption fallback, corruption and new storage generation");
}
'''
with tempfile.TemporaryDirectory(prefix='rc-node-pref-') as directory:
    d=Path(directory);(d/'stratosphere.hpp').write_text(header);(d/'main.cpp').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror',
       '-I'+str(d),'-I'+str(root/'console/ldn_mitm/source'),str(d/'main.cpp'),
       str(root/'console/ldn_mitm/source/node_preferences.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],cwd=d,check=True,timeout=10)
