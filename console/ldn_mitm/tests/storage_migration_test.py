#!/usr/bin/env python3
"""Run the native storage migration with actual files and injected SD failures."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'console/ldn_mitm/source/storage_migration.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
harness = r'''
#include "roomcircuit_paths.h"
#include "storage_migration.hpp"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <system_error>
namespace f = std::filesystem;
using u8 = uint8_t; using s64 = int64_t; using Result = int;
#define R_FAILED(x) ((x) != 0)
#define R_SUCCEEDED(x) ((x) == 0)
static std::string sd;
static int calls, fail_at;
static bool fail() { return fail_at == -1 || (fail_at && ++calls == fail_at); }
static std::string path(const char *p) { assert(!std::strncmp(p,"sdmc:/",6)); return sd + '/' + (p+6); }
namespace ams::util { int TSNPrintf(char *b, size_t n, const char *format, ...) { va_list ap;va_start(ap,format);int r=vsnprintf(b,n,format,ap);va_end(ap);return r; } }
namespace ams::fs {
struct ResultPathNotFound { static bool Includes(Result r) { return r==2; } };
enum DirectoryEntryType {DirectoryEntryType_Directory,DirectoryEntryType_File};
enum {OpenMode_Read=1,OpenMode_Write=2,OpenDirectoryMode_All=3};
struct FileHandle { void *handle; }; struct DirectoryHandle {void *handle;};
struct WriteOption { static const int None=0; };
Result GetEntryType(DirectoryEntryType *t,const char *p){if(fail())return 1;std::error_code e;auto s=f::status(path(p),e);if(e||!f::exists(s))return 2;*t=f::is_regular_file(s)?DirectoryEntryType_File:DirectoryEntryType_Directory;return 0;}
Result HasFile(bool *out,const char *p){if(fail())return 1;*out=f::is_regular_file(path(p));return 0;}
Result HasDirectory(bool *out,const char *p){if(fail())return 1;*out=f::is_directory(path(p));return 0;}
Result EnsureDirectory(const char *p){if(fail())return 1;std::error_code e;f::create_directories(path(p),e);return e?1:0;}
Result CreateDirectory(const char *p){if(fail())return 1;std::error_code e;return f::create_directory(path(p),e)?0:1;}
Result OpenDirectory(DirectoryHandle *h,const char *p,int){if(fail())return 1;h->handle=new std::string(path(p));return 0;}
Result GetDirectoryEntryCount(s64 *v,DirectoryHandle h){if(fail())return 1;*v=std::distance(f::directory_iterator(*static_cast<std::string*>(h.handle)),f::directory_iterator());return 0;}
void CloseDirectory(DirectoryHandle h){delete static_cast<std::string*>(h.handle);}
Result DeleteFile(const char *p){if(fail())return 1;std::error_code e;bool removed=f::remove(path(p),e);return e?1:removed?0:2;}
Result CreateFile(const char *p,s64 size){if(fail())return 1;if(f::exists(path(p)))return 1;std::ofstream o(path(p),std::ios::binary);if(!o)return 1;if(size){o.seekp(size-1);o.put('\0');}return o?0:1;}
Result OpenFile(FileHandle *h,const char *p,int mode){if(fail())return 1;FILE *file=fopen(path(p).c_str(),mode==OpenMode_Read?"rb":"r+b");if(!file)return 1;h->handle=file;return 0;}
Result GetFileSize(s64 *out,FileHandle h){if(fail())return 1;FILE *f=static_cast<FILE*>(h.handle);if(fseek(f,0,SEEK_END))return 1;*out=ftell(f);return *out<0;}
Result ReadFile(size_t *out,FileHandle h,s64 off,void *b,size_t n){if(fail())return 1;FILE *f=static_cast<FILE*>(h.handle);if(fseek(f,off,SEEK_SET))return 1;*out=fread(b,1,n,f);return ferror(f)?1:0;}
Result WriteFile(FileHandle h,s64 off,const void *b,size_t n,int){if(fail())return 1;FILE *f=static_cast<FILE*>(h.handle);return fseek(f,off,SEEK_SET)||fwrite(b,1,n,f)!=n;}
Result FlushFile(FileHandle h){if(fail())return 1;return fflush(static_cast<FILE*>(h.handle));}
void CloseFile(FileHandle h){fclose(static_cast<FILE*>(h.handle));}
Result RenameDirectory(const char *a,const char *b){if(fail())return 1;std::error_code e;f::rename(path(a),path(b),e);return e?1:0;}
}
/* SOURCE */
static void put(const std::string &name,const std::string &value){f::create_directories(f::path(sd+'/'+name).parent_path());std::ofstream(sd+'/'+name,std::ios::binary)<<value;}
static std::string read(const std::string &name){std::ifstream in(sd+'/'+name,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
static void reset(){f::remove_all(sd);f::create_directories(sd);calls=0;fail_at=0;}
int main(int argc,char **argv){assert(argc==2);sd=argv[1];using namespace ams::mitm::storage;
 reset();assert(Initialize()&&StorageUseCurrent());assert(f::exists(path(RC_STORAGE_MARKER)));
 reset();put("lanywhere/config.ini","server=1\n");put("lanywhere/language.ini.previous","language=zh\n");put("lanywhere/appointment-subscriptions.bin","saved");put("lanywhere/lanplay-ip.bin","identity");put("lanywhere/custom.txt","user file");
 calls=0;fail_at=999999;assert(Initialize());const int steps=calls;assert(steps>30);assert(read("switch/RoomCircuit/data/config.ini")=="server=1\n");assert(read("switch/RoomCircuit/data/language.ini.previous")=="language=zh\n");assert(read("lanywhere/custom.txt")=="user file");
 for(int i=1;i<=steps;i++){
  reset();put("lanywhere/config.ini","before");put("lanywhere/language.ini","language=zh\n");calls=0;fail_at=i;
  bool migrated=Initialize();if(!migrated){assert(!StorageUseCurrent());assert(read("lanywhere/config.ini")=="before");
   const bool committed=f::exists(path(RC_STORAGE_MARKER));
   if(committed) assert(!StorageAvailable()); // No service may write before retry.
   else put("lanywhere/config.ini","changed after interruption");
   calls=0;fail_at=0;assert(Initialize());
   assert(read("switch/RoomCircuit/data/config.ini")==(committed?"before":"changed after interruption"));}
 }
 reset();put("lanywhere/config.ini","old");put("switch/RoomCircuit/data/config.ini","user target");assert(!Initialize()&&!StorageAvailable());assert(read("switch/RoomCircuit/data/config.ini")=="user target" && read("lanywhere/config.ini")=="old");
 // A missing commit marker must never make previously migrated data silently
 // fall back to the stale legacy backup. After backup and marker restoration,
 // the same data is selected again without copying in either direction.
 reset();put("lanywhere/config.ini","stale");put("switch/RoomCircuit/data/config.ini","latest");
 assert(!Initialize()&&!StorageAvailable()&&StorageSelection()==Selection::Unavailable);
 assert(read("lanywhere/config.ini")=="stale"&&read("switch/RoomCircuit/data/config.ini")=="latest");
 put("switch/RoomCircuit/data/.storage-v1","");
 assert(Initialize()&&StorageSelection()==Selection::Current&&read("switch/RoomCircuit/data/config.ini")=="latest");
 reset();put("switch/RoomCircuit/config.ini","server=bootstrap");assert(Initialize());assert(read("switch/RoomCircuit/data/config.ini")=="server=bootstrap");
 reset();put("lanywhere/config.ini","old server");put("switch/RoomCircuit/config.ini","new server");assert(!Initialize()&&!StorageUseCurrent());assert(read("lanywhere/config.ini")=="old server"&&read("switch/RoomCircuit/config.ini")=="new server");
 reset();put("lanywhere/config.ini","same");put("switch/RoomCircuit/config.ini","same");assert(Initialize()&&read("switch/RoomCircuit/data/config.ini")=="same");
 reset();put("lanywhere/.update/journal","pending");put("lanywhere/config.ini","old");assert(!Initialize()&&!StorageUseCurrent());assert(!f::exists(path(RC_STORAGE_MARKER)));
 // Permanent status I/O failure is unknown, never a writeable legacy root.
 reset();put("switch/RoomCircuit/data/.storage-v1","");fail_at=-1;
 assert(!Initialize()&&!StorageAvailable()&&StorageSelection()==Selection::Unavailable);
 fail_at=0;assert(Initialize()&&StorageAvailable()&&StorageSelection()==Selection::Current);
 reset();put("switch/RoomCircuit/.data-v1-staging/foreign.txt","keep");assert(!Initialize()&&!StorageUseCurrent());assert(read("switch/RoomCircuit/.data-v1-staging/foreign.txt")=="keep");
 puts("PASS native storage: fresh, settings/sidecars, every SD error and interrupted retry, OTA journal, user conflicts, foreign staging");
}
'''.replace('/* SOURCE */', source)
with tempfile.TemporaryDirectory(prefix='roomcircuit-storage-test-') as temp:
    path = Path(temp)
    cpp, binary, sd = path/'test.cpp', path/'test', path/'sd'
    cpp.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'common'), '-I'+str(root/'console/ldn_mitm/source'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(sd)], check=True, timeout=30)
