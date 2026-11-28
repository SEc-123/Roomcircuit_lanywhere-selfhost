#include "../source/lanplay_codec.hpp"
#include "../source/lanplay_direct.hpp"
#include "../source/tcp_stream_protocol.hpp"
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace ams::mitm;
static uint64_t game;
static std::array<uint8_t,0x480> info{};
static std::mutex output_mutex;
static std::map<unsigned,unsigned> member_slots;
static std::map<unsigned,uint64_t> seqs;
static unsigned capacity=8;
static void Emit(const std::string &s) { std::scoped_lock lock(output_mutex); std::puts(s.c_str()); std::fflush(stdout); }
static uint32_t Hash(const void *raw,size_t size) {const auto*p=static_cast<const uint8_t*>(raw);uint32_t h=2166136261;for(size_t i=0;i<size;i++)h=(h^p[i])*16777619;return h;}
static std::vector<uint8_t> Payload(uint32_t ip,unsigned id,unsigned size) {std::vector<uint8_t>b(size);for(unsigned i=0;i<size;i++)b[i]=uint8_t((ip+i*17+id*31)%251);return b;}
static void Snapshot(const char*event) {const auto s=lanplay::State();Emit(std::string("{\"event\":\"")+event+"\",\"ip\":"+std::to_string(s.address)+",\"active\":"+(s.active?"true":"false")+",\"members\":"+std::to_string(s.members)+",\"slot\":"+std::to_string(s.member)+",\"epoch\":\""+std::to_string(s.epoch)+"\"}");}
static std::array<uint8_t,0x480> Network(uint32_t ip) {std::array<uint8_t,0x480>b{};tcpstream::Put64(b.data(),game);b[0x10]=3;b[0x1f]=9;b[0x20]=2;lanplay::Put32(b.data()+0x22,ip);b[0x66]=capacity;b[0x67]=1;lanplay::Put32(b.data()+0x68,ip);b[0x6c]=2;lanplay::Put32(b.data()+0x6e,ip);b[0x73]=1;std::memcpy(b.data()+0x74,"Native host",12);lanplay::Put16(b.data()+0x96,1);return b;}
static void Membership() {std::string s="{\"event\":\"members\",\"count\":"+std::to_string(info[0x67])+",\"ips\":[";bool first=true;for(unsigned i=0;i<8;i++)if(info[0x68+i*64+11]){if(!first)s+=",";first=false;s+=std::to_string(lanplay::Le32(info.data()+0x68+i*64));}Emit(s+"]}");}
static void Sync() {lanplay::Publish(info.data(),info.size());for(const auto&it:member_slots)lanplay::SendControl(it.first,4,info.data(),info.size());Membership();}
static void Found(const char*code,const void*raw,void*arg) {auto*p=static_cast<std::pair<std::string,std::array<uint8_t,0x480>>*>(arg);p->first=code;std::memcpy(p->second.data(),raw,p->second.size());}
static bool SendFrame(const tcpstream::Frame&f,uint64_t epoch) {uint8_t b[1100];const auto n=tcpstream::Encode(b,sizeof(b),f);return n&&lanplay::SendStream(epoch,b,n);}
static void Stream(uint64_t epoch,const void*raw,size_t size) {tcpstream::Frame f;assert(tcpstream::Parse(raw,size,&f));
 Emit("{\"event\":\"stream\",\"op\":"+std::to_string(unsigned(f.op))+",\"peer\":"+std::to_string(f.peer)+",\"id\":"+std::to_string(f.id)+",\"owner\":"+std::to_string(f.owner)+",\"seq\":"+std::to_string(f.seq)+",\"size\":"+std::to_string(f.size)+",\"hash\":"+std::to_string(Hash(f.data,f.size))+"}");
 tcpstream::Frame r=f;r.owner=f.owner?0:1;r.data=nullptr;r.size=0;r.source_port=r.destination_port=0;r.seq=0;
 if(f.op==tcpstream::Op::Open){r.op=tcpstream::Op::OpenOk;r.aux=8192;assert(SendFrame(r,epoch));}
 else if(f.op==tcpstream::Op::Data){r.op=tcpstream::Op::Window;r.seq=f.seq+f.size;r.aux=8192;assert(SendFrame(r,epoch));}
 else if(f.op==tcpstream::Op::Fin){r.op=tcpstream::Op::Fin;r.seq=f.seq;r.aux=0;SendFrame(r,epoch);}
}
static void Controls() {uint8_t from,kind,b[0x500];ssize_t n;while((n=lanplay::TakeControl(&from,&kind,b,sizeof(b)))>=0){
 Emit("{\"event\":\"control\",\"from\":"+std::to_string(from)+",\"kind\":"+std::to_string(kind)+",\"size\":"+std::to_string(n)+"}");
 if(lanplay::State().hosting&&kind==3&&n==64){unsigned slot=1;const uint32_t ip=lanplay::Le32(b);while(slot<8&&info[0x68+slot*64+11]&&lanplay::Le32(info.data()+0x68+slot*64)!=ip)slot++;
  if(slot>=capacity){lanplay::SendControl(from,5,nullptr,0);Emit("{\"event\":\"rejected\"}");continue;}
  if(!info[0x68+slot*64+11])info[0x67]++;std::memcpy(info.data()+0x68+slot*64,b,64);info[0x68+slot*64+10]=slot;member_slots[from]=slot;Sync();
 }else if(kind==4&&n==0x480){std::memcpy(info.data(),b,n);Membership();}
 else if(kind==255&&lanplay::State().hosting){auto it=member_slots.find(from);if(it!=member_slots.end()){std::memset(info.data()+0x68+it->second*64,0,64);info[0x67]--;member_slots.erase(it);Sync();}}
 else if(kind==255)Snapshot("disconnected");
}}
int main(int argc,char**argv){assert(argc==2);const unsigned port=std::strtoul(argv[1],nullptr,0);assert(port>0&&port<65536);lanplay::Endpoint endpoint{htonl(INADDR_LOOPBACK),htons(port)};assert(lanplay::Configure(2,endpoint));lanplay::SetStreamReply(Stream);fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);Snapshot("ready");std::string input;bool running=true;
 while(running){char chunk[4096];const auto n=read(STDIN_FILENO,chunk,sizeof(chunk));if(n>0)input.append(chunk,n);size_t end;
 while((end=input.find('\n'))!=std::string::npos){const std::string line=input.substr(0,end);input.erase(0,end+1);std::istringstream cmd(line);std::string op;cmd>>op;
 if(op=="HOST"){std::string g;cmd>>g>>capacity;game=std::strtoull(g.c_str(),nullptr,0);assert(lanplay::Host(game));info=Network(lanplay::State().address);member_slots.clear();seqs.clear();lanplay::Publish(info.data(),info.size());Snapshot("host");Membership();}
 else if(op=="JOIN"){std::string g;uint32_t host;unsigned flavor;cmd>>g>>host>>flavor;game=std::strtoull(g.c_str(),nullptr,0);auto target=Network(host);NodeRoomLocator selected{};std::memcpy(selected.network_id,target.data(),32);std::memcpy(selected.bssid,target.data()+0x20,6);selected.flavor=flavor;selected.host_ip=host;std::pair<std::string,std::array<uint8_t,0x480>> found;uint8_t filter[96]{};tcpstream::Put64(filter,game);lanplay::Put32(filter+92,1);const int count=lanplay::Scan(filter,sizeof(filter),&selected,Found,&found);bool ok=count>0&&lanplay::Join(found.first.c_str(),game);if(ok){info=found.second;uint8_t node[64]{};lanplay::Put32(node,lanplay::State().address);node[4]=2;node[11]=1;std::memcpy(node+12,"Native guest",13);lanplay::Put16(node+46,1);ok=lanplay::SendControl(0,3,node,sizeof(node));}Emit("{\"event\":\"join\",\"ok\":"+std::string(ok?"true":"false")+",\"found\":"+std::to_string(count)+"}");}
 else if(op=="UDP"){uint32_t dest;unsigned id,size;cmd>>dest>>id>>size;auto b=Payload(lanplay::State().address,id,size);const bool ok=lanplay::SendData(dest,46000+id,45000,b.data(),b.size());Emit("{\"event\":\"sentudp\",\"id\":"+std::to_string(id)+",\"ok\":"+(ok?"true":"false")+"}");}
 else if(op=="OPEN"){uint32_t dest;unsigned id;cmd>>dest>>id;auto s=lanplay::State();tcpstream::Frame f;f.peer=dest;f.id=id;f.nonce=s.epoch;f.op=tcpstream::Op::Open;f.source_port=48000+id;f.destination_port=6500;f.aux=8192;f.data=info.data();f.size=32;const bool ok=SendFrame(f,s.epoch);Emit("{\"event\":\"sentopen\",\"id\":"+std::to_string(id)+",\"ok\":"+(ok?"true":"false")+"}");}
 else if(op=="DATA"){uint32_t dest;unsigned id,size;cmd>>dest>>id>>size;auto s=lanplay::State();auto b=Payload(s.address,id,size);tcpstream::Frame f;f.peer=dest;f.id=id;f.nonce=s.epoch;f.op=tcpstream::Op::Data;f.seq=seqs[id];f.data=b.data();f.size=b.size();const bool ok=SendFrame(f,s.epoch);if(ok)seqs[id]+=size;Emit("{\"event\":\"sentdata\",\"id\":"+std::to_string(id)+",\"ok\":"+(ok?"true":"false")+"}");}
 else if(op=="LEAVE"){lanplay::Leave();member_slots.clear();seqs.clear();Snapshot("left");}
 else if(op=="STATE")Snapshot("state");
 else if(op=="QUIT")running=false;
 else assert(false);
 }
 Controls();uint8_t b[65507];uint32_t src;uint16_t sport;ssize_t size;while((size=lanplay::ReceiveData(45000,b,sizeof(b),&src,&sport,false))>=0)Emit("{\"event\":\"udp\",\"peer\":"+std::to_string(src)+",\"id\":"+std::to_string(sport-46000)+",\"size\":"+std::to_string(size)+",\"hash\":"+std::to_string(Hash(b,size))+"}");std::this_thread::sleep_for(std::chrono::milliseconds(2));
 }lanplay::Shutdown();Emit("{\"event\":\"shutdown\"}");return 0;}
