#!/usr/bin/env python3
"""Actual UDP BSD hooks, portable Horizon wire and real loopback kernel descriptors."""
from pathlib import Path
import importlib.util
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('slices', ROOT/'app/tests/input_routing_test.py')
slices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(slices)
source = (ROOT/'console/ldn_mitm/source/bsd_spy.cpp').read_text()
base = (Path(__file__).with_name('tcp_stream_ipc_harness.cpp.in')).read_text().split('using ams::mitm::bsd::BsdSpyService;')[0]
base = base.replace('#include "tcp_stream_hooks.hpp"', '#include "tcp_stream_hooks.hpp"\n#include "virtual_receive.hpp"\n#include "udp_port_policy.hpp"\n#include "bsd_ipc_abi.hpp"\n#include "relay_queue.hpp"')
base = base.replace('struct GameNamespace {', 'struct GameNamespace {\n std::function<void(u32)> after_dispatch;')
base = base.replace('  std::memcpy(output,&out,output_size);', '  if (self.after_dispatch) self.after_dispatch(command);\n  std::memcpy(output,&out,output_size);')
base = base.replace('&&ints[1]==1);const int real=::socket(AF_INET,SOCK_STREAM,0)', '&&(ints[1]==1||ints[1]==2));const int real=::socket(AF_INET,ints[1],0)')
base = base.replace('const auto e=endpoint(0);if(command==14', '''tcpstream::Endpoint e{};
    const auto *wire=static_cast<const uint8_t*>(buffers[0].data);
    const bool disconnected=command==14&&buffers[0].size>=2&&wire[1]==AF_UNSPEC;
    if(!disconnected)e=endpoint(0);
    if(command==14''')
base = base.replace('a.sin_family=AF_INET;a.sin_addr', 'a.sin_family=disconnected?AF_UNSPEC:AF_INET;a.sin_addr',1)
base = base.replace('self.virtual_connects++;sockaddr_in a{};', 'self.virtual_connects++;\n    sockaddr_in a{};',1)
base = base.replace('out.ret=command==13?::bind(fd', '\n#ifdef __APPLE__\n a.sin_len=sizeof(a);\n#endif\n out.ret=command==13?::bind(fd',1)
anchor='   case 12:'
base=base.replace(anchor, r'''
   case 8:case 9:{
    assert(input_size==8&&count==(command==8?1u:2u));
    int flags=((ints[1]&0x80)?MSG_DONTWAIT:0)|((ints[1]&2)?MSG_PEEK:0);
    sockaddr_in address{};socklen_t length=sizeof(address);
    out.ret=::recvfrom(fd,buffers[0].data,buffers[0].size,flags,reinterpret_cast<sockaddr*>(&address),&length);
    if(out.ret>=0&&command==9){auto wire=tcpstream::MakeAddress({ntohl(address.sin_addr.s_addr),ntohs(address.sin_port)});std::memcpy(buffers[1].data,&wire,std::min(buffers[1].size,sizeof(wire)));out.length=sizeof(wire);}
    break;
   }
   case 10:case 11:{
    assert(input_size==8&&count==(command==10?1u:2u));
    const int flags=MSG_DONTWAIT;
    if(command==10)out.ret=::send(fd,buffers[0].data,buffers[0].size,flags);
    else{const auto e=endpoint(1);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(e.ip);address.sin_port=htons(e.port);out.ret=::sendto(fd,buffers[0].data,buffers[0].size,flags,reinterpret_cast<sockaddr*>(&address),sizeof(address));}
    break;
   }
''' +anchor,1)
# Match the full source fields and methods rather than the old TCP-only inert UDP seam.
base=base.replace('static const char *FormatAddr(char *out,size_t,const void*,size_t){*out=0;return out;}', 'static const char *FormatAddr(char *out,size_t,const void*,size_t){*out=0;return out;}')
base=base.replace('constexpr s32 ErrnoAddr=99;struct{int poll_calls=0,poll_ready=0;}g_recv;', '''constexpr s32 ErrnoAddr=99,ErrnoAgain=11,ErrnoIo=5,ErrnoFamily=97,ErrnoBuffers=105,ErrnoHost=113;
constexpr s32 BsdGetFl=3,BsdPeek=2,BsdDontWait=0x80;
struct{u32 recv_calls=0,recv_data=0,recv_eagain=0,recvfrom_calls=0,recvfrom_data=0,recvfrom_eagain=0,poll_calls=0,poll_ready=0;u64 bytes=0;}g_recv;
static u32 g_overflow=0;
struct sockaddr_in{u8 sin_len,sin_family;u16 sin_port;in_addr sin_addr;u8 padding[8];};
''')
base=base.replace('namespace sm {struct Number', 'namespace socket {enum class Level{Sol_Socket=0xffff};enum class Option{So_RcvTimeo=0x1006};}\nnamespace sm {struct Number')
base=base.replace('struct Tick{std::chrono::steady_clock::time_point at;}', 'struct Tick{std::chrono::steady_clock::time_point at;TimeSpan ToTimeSpan()const{return {std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count()};}}')
base=base.replace('#define R_SUCCEED() return 0','#define R_SUCCEED() return 0\n#define R_FAILED(x) ((x)!=0)')
a=base.index('namespace mitm::lanywhere {');b=base.index('namespace mitm::tcpstream {',a)
base=base[:a]+r'''
namespace mitm::lanywhere {
static bool room=true;static u64 room_epoch=1;static RelayQueue queue;static int unavailable=0;static u32 sent_ip;static u16 sent_port,sent_source;static std::vector<u8> sent_data;
static bool InRoom(){return room;}static u64 UdpRoomEpoch(){return room ? room_epoch : 0;}static bool IsVirtual(u32 ip){return (ip>>16)==0x0a0d;}
static bool HasData(u16 port,u32 peer=0,u16 source=0){return queue.Has(port,peer,source);}
static ssize_t ReceiveData(u16 port,void *data,size_t size,u32 *ip,u16 *src,bool peek,u32 peer=0,u16 source=0){return queue.Take(port,data,size,ip,src,peek,peer,source);}
static bool SendData(u32 ip,u16 src,u16 dst,const void *data,size_t size){sent_ip=ip;sent_port=dst;sent_source=src;sent_data.assign(static_cast<const u8*>(data),static_cast<const u8*>(data)+size);return true;}
static void ReportTcpUnavailable(){unavailable++;}
}
''' +base[b:]
a=base.index(' Result EnsureUdpPort(');b=base.index('\n};\n#define Global',a)
base=base[:a]+'''
 Result EnsureUdpPort(s32,u16*,s32*,bool);
 Result ReceiveVirtual(s32,s32,const sf::OutAutoSelectBuffer&,const sf::OutAutoSelectBuffer*,bool*,s32*,s32*,u32*);
 Result Socket(sf::Out<s32>,sf::Out<s32>,s32,s32,s32);
 Result Bind(sf::Out<s32>,sf::Out<s32>,s32,const sf::InAutoSelectBuffer&);
 Result Write(sf::Out<s32>,sf::Out<s32>,s32,const sf::InAutoSelectBuffer&);
 Result Recv(sf::Out<s32>,sf::Out<s32>,s32,s32,const sf::OutAutoSelectBuffer&);
 Result RecvFrom(sf::Out<s32>,sf::Out<s32>,sf::Out<u32>,s32,s32,const sf::OutAutoSelectBuffer&,const sf::OutAutoSelectBuffer&);
 Result Send(sf::Out<s32>,sf::Out<s32>,s32,s32,const sf::InAutoSelectBuffer&);
 Result SendTo(sf::Out<s32>,sf::Out<s32>,s32,s32,const sf::InAutoSelectBuffer&,const sf::InAutoSelectBuffer&);
''' +base[b:]
fields=source[source.index('        struct Destination {'):source.index('        os::Tick g_last_summary;')]
helpers='\n'.join(slices.function(source,x) for x in ['SocketStats *Find(', 'void OpenSocketOwner(', 'void CloseSocketOwner(', 'template<class Update> void UpdateAliases(', 'bool DuplicateUdp(', 'SocketStats UdpSocket('])
helpers += source[source.index('        struct UdpWatch {'):source.index('        void Summary(')]
base=base.replace('/* UDP_TABLE_SOURCE */',fields+'\n'+helpers)
methods=['EnsureUdpPort','Socket','Poll','Select','Recv','RecvFrom','ReceiveVirtual','Fcntl','SetSockOpt','Send','SendTo','Bind','Connect','Listen','Close','Accept','GetPeerName','GetSockName','GetSockOpt','Shutdown','ShutdownAllSockets','DuplicateSocket','Read','Write','RecvMMsg','SendMMsg']
base=base.replace(' Result Socket(', ' Result Listen(sf::Out<s32>,sf::Out<s32>,s32,s32);\n Result Socket(',1)
functions='\n'.join(slices.function(source,s) for s in ['tcpstream::Owner TcpOwner(','u32 TcpDispatch(','void TcpAddress(','ssize_t TakeRoomPacket(const SocketStats &socket, void *buffer, size_t size, sockaddr_in *from, s32 flags) {','BsdSpyService::BsdSpyService(','BsdSpyService::~BsdSpyService(']+['Result BsdSpyService::'+name+'(' for name in methods])
base=base.replace('/* ACTUAL_SOURCE */',functions).replace('/* BSD_PREDICATE */','static bool ShouldMitm(const sm::MitmProcessInfo&){return true;}')
sdk=(ROOT/'console/Atmosphere-libs/libstratosphere/include/stratosphere/ncm/ncm_content_meta_id.hpp').read_text()
base=base.replace('/* SDK_PREDICATE */','')
main=Path(__file__).with_name('udp_bsd_ipc_harness.cpp.in').read_text()
with tempfile.TemporaryDirectory(prefix='udp-actual-ipc-') as td:
 td=Path(td);cpp=td/'udp.cpp';exe=td/'udp';cpp.write_text(base+main)
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(ROOT/'console/ldn_mitm/source'),str(cpp),str(ROOT/'console/ldn_mitm/source/tcp_stream_proxy.cpp'),str(ROOT/'console/ldn_mitm/source/tcp_stream_hooks.cpp'),'-o',str(exe)],check=True,timeout=45)
 subprocess.run([str(exe)],check=True,timeout=20)
