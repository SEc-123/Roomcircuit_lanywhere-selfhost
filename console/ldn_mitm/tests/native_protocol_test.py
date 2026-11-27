#!/usr/bin/env python3
"""Exercise actual physical LAN framing with bounded peer/socket inputs."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
native=ROOT/'console/ldn_mitm/source'
header=(native/'lan_protocol.hpp').read_text()
source=(native/'lan_protocol.cpp').read_text()
def strip(text):
    return '\n'.join(line for line in text.splitlines() if not line.startswith('#include') and line!='#pragma once')
harness=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;
#define LogFormat(...) ((void)0)
#define LogHex(...) ((void)0)
#define AMS_UNUSED(x) (void)(x)
static std::vector<u8> sent;
static bool fail_send=false;
ssize_t test_sendto(int,const void *data,size_t count,int,const sockaddr*,socklen_t){
 if(fail_send){errno=EPIPE;return -1;}
 const size_t n=std::min<size_t>(count,3);
 const auto *bytes=static_cast<const u8*>(data);sent.insert(sent.end(),bytes,bytes+n);return n;
}
static int poll_error;
int test_poll(pollfd *fds,nfds_t n,int timeout){
 if(poll_error){errno=poll_error;return -1;}
 return ::poll(fds,n,timeout);
}
#define poll test_poll
#define sendto test_sendto
/* HEADER */
/* SOURCE */
#undef sendto
#undef poll
class Stream:public LanSocket {public:
 std::vector<u8> incoming;unsigned receives=0;
 Stream():LanSocket(-1){}
 bool isStream()const override{return true;}
 ssize_t recvfrom(void *out,size_t count,sockaddr_in*)override{
  ++receives;if(incoming.empty()){errno=EAGAIN;return -1;}
  size_t n=std::min(incoming.size(),count);std::memcpy(out,incoming.data(),n);incoming.erase(incoming.begin(),incoming.begin()+n);return n;
 }
 int test_sendto(const void*,size_t,sockaddr_in*)override{return -1;}
 using LanSocket::compress;using LanSocket::decompress;
};
class Tcp:public TcpLanSocketBase {public:Tcp():TcpLanSocketBase(-1){}using TcpLanSocketBase::test_sendto;};
std::vector<u8> frame(const std::vector<u8>&body){
 LANPacketHeader h{};h.magic=0x11451400;h.type=LANPacketType::Connect;h.length=body.size();
 std::vector<u8>b(sizeof(h)+body.size());std::memcpy(b.data(),&h,sizeof(h));std::memcpy(b.data()+sizeof(h),body.data(),body.size());return b;
}
int main(){
 poll_error=EINTR;assert(Pollable::Poll(nullptr,0,0)==0);
 poll_error=EIO;assert(Pollable::Poll(nullptr,0,0)<0);
 poll_error=0;assert(Pollable::Poll(nullptr,0,0)==0);
 Stream socket;
 for(size_t n:{size_t(1),size_t(2),size_t(256),size_t(1152)}){
  auto input=std::make_unique<u8[]>(n);std::memset(input.get(),0,n);
  std::vector<u8> compressed(n*2),out(n);size_t compressed_size=compressed.size();
  assert(socket.compress(input.get(),n,compressed.data(),&compressed_size)==0);
  size_t size=n;assert(socket.decompress(compressed.data(),compressed_size,out.data(),&size)==0&&size==n);
  assert(!std::memcmp(input.get(),out.data(),n));
 }
 u8 overlong[]={0,3},out[3];size_t size=sizeof(out);assert(socket.decompress(overlong,2,out,&size)!=0);
 auto first=frame({1,2,3}),second=frame({4,5,6});socket.incoming=first;socket.incoming.insert(socket.incoming.end(),second.begin(),second.end());
 unsigned callbacks=0;
 assert(socket.recvPacket([&](LANPacketType type,const void*bytes,size_t n,ReplyFunc){assert(type==LANPacketType::Connect&&n==3);assert(static_cast<const u8*>(bytes)[0]==(callbacks?4:1));callbacks++;return 0;})==0);
 assert(callbacks==2&&socket.receives==1);
 Stream fragmented;fragmented.incoming.assign(first.begin(),first.begin()+3);callbacks=0;
 auto consume=[&](LANPacketType,const void*,size_t,ReplyFunc){callbacks++;return 0;};
 assert(fragmented.recvPacket(consume)==0&&!callbacks);
 fragmented.incoming.assign(first.begin()+3,first.end());assert(fragmented.recvPacket(consume)==0&&callbacks==1);
 Tcp tcp;std::vector<u8> bytes(100,7);assert(tcp.test_sendto(bytes.data(),bytes.size(),nullptr)==100&&sent==bytes);
 fail_send=true;assert(tcp.test_sendto(bytes.data(),bytes.size(),nullptr)<0);
 std::vector<u8> too_large(2048);assert(socket.sendPacket(LANPacketType::ScanResp,too_large.data(),too_large.size())<0);
 puts("PASS actual physical LAN protocol: terminal-zero compression, strict expansion bounds, coalesced/partial frames and short-send completion");
}
'''
code=harness.replace('/* HEADER */',strip(header)).replace('/* SOURCE */',strip(source))
with tempfile.TemporaryDirectory(prefix='native-protocol-') as tmp:
 cpp=Path(tmp)/'test.cpp';cpp.write_text(code);binary=Path(tmp)/'test'
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=10)
