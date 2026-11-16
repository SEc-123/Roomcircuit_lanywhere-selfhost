#!/usr/bin/env python3
"""Preserve flag-gated scan fields and pre-network advertising through actual LDN entrypoints."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--report', type=Path)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('slices', ROOT / 'app/tests/input_routing_test.py')
slices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(slices)
native = ROOT / 'console/ldn_mitm/source'
source = (native / 'lan_discovery.cpp').read_text()
cache = (native / 'game_identity_cache.cpp').read_text()
relay = (native / 'lanywhere_relay.cpp').read_text()
header = (native / 'lanywhere_relay.hpp').read_text()
functions = '\n'.join(slices.function(source, signature) for signature in [
    'static bool MatchesScanFilter(', 'Result LANDiscovery::resolveGameLcid(',
    'Result LANDiscovery::scan(', 'void LANDiscovery::onPublicGroup(', 'void LANDiscovery::leaveAutoRoom(',
    'Result LANDiscovery::createNetwork(', 'Result LANDiscovery::connect(',
    'Result LANDiscovery::openAccessPoint(', 'Result LANDiscovery::setAdvertiseData(',
    'Result LANDiscovery::initNetworkInfo(',
])
exact = '\n'.join(slices.function(cache, signature) for signature in [
    'bool ReadAt(', 'GameCacheResult GameIdentityCacheExactRecord(',
])
routes = []
for name in ('DiscoveryRoute', 'GameRoute', 'GroupSummaryV2'):
    start = header.index('struct ' + name + ' {')
    routes.append(header[start:header.index('};', start) + 2])
relay_functions = '\n'.join(slices.function(relay, signature) for signature in [
    'u16 le16(', 'u32 le32(', 'u64 le64(',
    'bool MakeMatchFilter(', 'bool MatchesFilter(', 'void AddCachedIdentity(',
])
template = Path(__file__).with_name('lcid_entry_harness.cpp.in').read_text().split('int main(){', 1)[0]
template = template.replace('class LANDiscovery {public:', 'enum class DisconnectReason {None};\nclass LANDiscovery {public:')
template = template.replace('#include "node_lobby.h"', '#include "node_lobby.h"\n#include "lanplay_codec.hpp"')
template = template.replace('int getFd(){return -1;}', 'void close(){}int getFd(){return -1;}')
template = template.replace('Result initNetworkInfo(){networkInfo={};return 0;}', 'Result initNetworkInfo();')
template = template.replace(' Result resolveGameLcid(', ''' static constexpr const char *FakeSsid="12345678123456781234567812345678";
 Result getFakeMac(MacAddress*mac){*mac={};mac->raw[0]=2;return 0;}
 void roomCloseStations(){}void resetStations(){}
 unsigned publications=0;DisconnectReason disconnect_reason=DisconnectReason::None;
 Result openAccessPoint();Result setAdvertiseData(const u8*,uint16_t);
 Result resolveGameLcid(''')
template = template.replace('void updateNodes(){published=networkInfo;}', 'void updateNodes(){published=networkInfo;++publications;}')
template = template.replace('constexpr int ModuleID=0xfd,LdnModuleId=0xcb;', 'constexpr int ModuleID=0xfd,LdnModuleId=0xcb,LdnAcceptPolicy_AlwaysAccept=0;')
direct = slices.function((native / 'lanplay_direct.cpp').read_text(), 'bool MatchesScan(')
template += '\nnamespace ams::mitm::lanplay { struct DirectFilter { bool scan_has_selection=false,scan_has_filter=true;NodeRoomLocator scan_selection{};uint8_t scan_filter[96]{};'+direct+'}; }\n'
harness = template + r'''
int main(int argc,char**argv){
 assert(argc==2);
 constexpr u64 program=0x01008f6008c5e000ULL,lcid=0x0100a3d008c5c000ULL;
 const auto record=make_record(program,lcid);
 if(!std::strcmp(argv[1],"scan")){
  unsigned cases=0;
  // Every supported combination with SSID disabled must ignore all SSID bytes.
  // Cover self relay, original community, direct LAN Play and physical fallback.
  for(const char *provider:{"self","lanywhere-public","lanplay-cn-jax","physical"}){
   for(u32 flags=0;flags<=0x27;flags++){
    if(flags&~0x27u)continue;
    reset(!std::strcmp(provider,"physical")?"self":provider);encode({record});
    if(!std::strcmp(provider,"physical"))lanywhere::current.discovery.public_enabled=false;
    LANDiscovery game;game.hostProgramId=program;game.state=CommState::Station;
    ScanFilter filter;std::memset(&filter,0xa5,sizeof(filter));
    filter.flag=flags;filter.networkId.intentId.localCommunicationId=lcid;filter.networkType=2;
    filter.ssid.length=255;
    have_offer=true;offered.networkId=filter.networkId;offered.common.networkType=2;
    offered.common.ssid="ordinary-room";offered.ldn.nodeCount=1;
    NetworkInfo result[8]{};u16 count=8;
    const auto rc=game.scan(result,&count,filter);
    if(rc||count!=1){std::printf("FAIL unused SSID provider=%s flags=%u result=%d count=%u network_calls=%d\n",provider,flags,rc,count,page_calls+physical_calls);return 1;}
    assert(result[0].networkId.intentId.localCommunicationId==lcid);
    assert(page_calls+physical_calls==1);++cases;
   }
  }
  // Enabling SSID validation still rejects invalid lengths before discovery.
  for(u8 length:{u8(33),u8(255)}){
   reset();encode({record});LANDiscovery game;game.hostProgramId=program;
   ScanFilter filter{};filter.flag=ScanFilterFlag_Ssid;filter.ssid.length=length;
   NetworkInfo result[8]{};u16 count=8;
   assert(game.scan(result,&count,filter)!=0&&count==0&&!effects);
  }
  std::printf("PASS %u flag-gated scan cases and active SSID validation\n",cases);
 }else if(!std::strcmp(argv[1],"network")){
  for(u32 mask:{u32(0),u32(1),u32(2),u32(3),u32(255),u32(0x80000000),u32(0xffffffff)}){
   for(u8 kind:{u8(1),u8(2)}){
    ScanFilter filter{};filter.flag=ScanFilterFlag_LocalCommunicationId|ScanFilterFlag_NetworkType;
    filter.networkId.intentId.localCommunicationId=lcid;filter.networkType=mask;
    filter.ssid.length=255;
    NetworkInfo info{};info.networkId=filter.networkId;info.common.networkType=kind;
    u8 encoded[96]{};assert(lanywhere::MakeMatchFilter(filter,encoded));
    encoded[42]=255; // Disabled SSID bytes are irrelevant in both wire matchers.
    lanplay::DirectFilter direct;std::memcpy(direct.scan_filter,encoded,sizeof(encoded));
    const bool expected=(mask&kind)!=0;
    const bool public_match=MatchesScanFilter(filter,info);
    const bool reservation_match=lanywhere::MatchesFilter(info,encoded);
    const bool direct_match=direct.MatchesScan(reinterpret_cast<const u8*>(&info),0,NodeFlavorStandard);
    if(public_match!=expected||reservation_match!=expected||direct_match!=expected){
     std::printf("FAIL network mask=%u kind=%u expected=%d public=%d reserved=%d direct=%d\n",mask,kind,expected,public_match,reservation_match,direct_match);return 1;
    }
   }
  }
  ScanFilter filter{};filter.flag=ScanFilterFlag_LocalCommunicationId|ScanFilterFlag_Ssid;
  filter.networkId.intentId.localCommunicationId=lcid;filter.ssid.length=33;
  u8 encoded[96]{};assert(!lanywhere::MakeMatchFilter(filter,encoded));
  filter.ssid.length=32;std::memset(filter.ssid.raw,'x',32);assert(lanywhere::MakeMatchFilter(filter,encoded));
  NetworkInfo info{};info.networkId=filter.networkId;info.common.ssid=filter.ssid;
  assert(lanywhere::MatchesFilter(info,encoded));
  encoded[42]=33;info.common.ssid.length=33;
  assert(!lanywhere::MatchesFilter(info,encoded));
  puts("PASS mask intersections in public/reserved/direct filters and active SSID bounds");
 }else{
  assert(!std::strcmp(argv[1],"advertise"));
  reset();encode({record});LANDiscovery game;game.hostProgramId=program;
  // Initialization is exercised in native_lifecycle_test; start from its actual
  // Initialized state, then invoke the production AP/data/create methods.
  game.state=CommState::Initialized;
  assert(game.openAccessPoint()==0&&game.state==CommState::AccessPoint);
  const u8 data[]={0x44,0x55,0x43,0x4b,0,0x19};
  const auto rc=game.setAdvertiseData(data,sizeof(data));
  if(rc){std::printf("FAIL pre-network advertise state=%u result=%d\n",unsigned(game.state),rc);return 1;}
  assert(game.publications==0&&game.networkInfo.ldn.advertiseDataSize==sizeof(data));
  SecurityConfig security{};UserConfig user{};NetworkConfig config{};
  config.intentId.localCommunicationId=lcid;config.nodeCountMax=4;config.localCommunicationVersion=1;
  assert(game.createNetwork(&security,&user,&config)==0&&game.state==CommState::AccessPointCreated);
  assert(game.publications==1&&published.ldn.advertiseDataSize==sizeof(data));
  assert(!std::memcmp(published.ldn.advertiseData,data,sizeof(data)));
  const u8 next[]={0x58,0x59};
  assert(game.setAdvertiseData(next,sizeof(next))==0&&game.publications==2);
  assert(published.ldn.advertiseDataSize==sizeof(next)&&!std::memcmp(published.ldn.advertiseData,next,sizeof(next)));
  assert(game.setAdvertiseData(nullptr,0)==0&&published.ldn.advertiseDataSize==0);
  u8 maximum[AdvertiseDataSizeMax];std::memset(maximum,0xa5,sizeof(maximum));
  assert(game.setAdvertiseData(maximum,sizeof(maximum))==0&&published.ldn.advertiseDataSize==sizeof(maximum));
  assert(!std::memcmp(published.ldn.advertiseData,maximum,sizeof(maximum)));
  assert(game.setAdvertiseData(nullptr,1)!=0);
  u8 too_large[AdvertiseDataSizeMax+1]{};assert(game.setAdvertiseData(too_large,sizeof(too_large))!=0);
  for(auto state:{CommState::None,CommState::Initialized,CommState::Station,CommState::StationConnected,CommState::Error}){
   game.state=state;const auto before=game.publications;
   assert(game.setAdvertiseData(data,sizeof(data))!=0&&game.publications==before);
  }
  puts("PASS pre-network advertise, exact data after create/publish, live replacement, clear and invalid-state/size rejection");
 }
}
'''
with tempfile.TemporaryDirectory(prefix='ldn-input-contract-') as temp:
    temp = Path(temp)
    (temp/'stratosphere.hpp').write_text('#pragma once\n#include <cstdint>\nusing u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using s64=int64_t;namespace ams::sf{struct LargeData{};struct PrefersPointerTransferMode{};}\n')
    cpp, binary = temp/'test.cpp', temp/'test'
    cpp.write_text(harness.replace('/* ACTUAL_ROUTES */','\n'.join(routes)).replace('/* ACTUAL_CACHE */',exact)
                   .replace('/* ACTUAL_RELAY */',relay_functions).replace('/* ACTUAL_ENTRIES */',functions))
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+[
        '-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(temp),'-I'+str(ROOT/'common'),'-I'+str(native),
        str(cpp),str(native/'ldn_types.cpp'),'-o',str(binary)
    ],check=True,timeout=40)
    results=[]
    for case in ('scan','advertise','network'):
        completed=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=10)
        print(completed.stdout,end='');print(completed.stderr,end='')
        results.append({'case':case,'passed':completed.returncode==0,'exit_code':completed.returncode,'output':completed.stdout+completed.stderr})
inputs = [native / name for name in ('lan_discovery.cpp', 'lanywhere_relay.cpp', 'lanplay_direct.cpp', 'public_scan_pages.hpp', 'ldn_types.cpp', 'ldn_types.hpp')]
report={'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
        'source_files_sha256':{str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
        'scope':'actual native entry/filter functions with controlled filesystem and transport boundaries',
        'hardware_tested':False,'cases':results,'passed':all(item['passed'] for item in results)}
if args.report:
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
assert report['passed'],[item['case'] for item in results if not item['passed']]
