'use strict';
const assert=require('node:assert/strict'),dgram=require('node:dgram'),{spawn}=require('node:child_process');
const {setTimeout:delay}=require('node:timers/promises');
const L=require('../../../server/relay/lanplay'),{Emu,nodeInfo}=require('../../../server/relay/lanplay-probe-peer');
const publicParts = process.env.LANPLAY_TEST_ENDPOINT?.split(':');
const publicEndpoint = publicParts ? {host:publicParts[0],port:Number(publicParts[1])} : null;
const {randomBytes}=require('node:crypto');
const GAME=process.env.LANPLAY_TEST_GAME ? BigInt(process.env.LANPLAY_TEST_GAME) : publicEndpoint ?
 0x0100c00000000000n | BigInt(randomBytes(4).readUInt32LE()) : 0x0100123456789000n;
process.env.LANPLAY_TEST_GAME='0x'+GAME.toString(16);
function fixtureAddress(fallback){
 if(!publicEndpoint)return fallback;
 let suffix;do{suffix=randomBytes(2).readUInt16LE();}while(!suffix||suffix===65535||suffix===0x2501||suffix===0xfefe);
 return 0x0a0d0000|suffix;
}
async function until(check,ms=8000){if(publicEndpoint)ms=Math.max(ms,20000);const end=Date.now()+ms;while(Date.now()<end){if(check())return;await delay(15);}throw Error('condition timed out');}
async function run(mode){
 const router=dgram.createSocket('udp4'),clients=new Map(),ips=new Map();let forwarded=0,maxPacket=0,dropped=false;
 router.on('message',(packet,peer)=>{
  const key=peer.address+':'+peer.port;clients.set(key,peer);maxPacket=Math.max(maxPacket,packet.length);
  if(packet[0]===2){router.send(packet,peer.port,peer.address);return;}
  let source,destination;
  if(packet[0]===1&&packet.length>=21){source=packet.readUInt32BE(13);destination=packet.readUInt32BE(17);}
  else if(packet[0]===3&&packet.length>=17){source=packet.readUInt32BE(1);destination=packet.readUInt32BE(5);}
  else return;
  if(packet[0]===1){const ip=L.parseIpv4(packet.subarray(1));if(ip&&ip.proto===6&&ip.body.length>20&&ip.body.readUInt16BE(2)===6500&&!dropped){dropped=true;return;}}
  ips.set(source,key);const target=ips.get(destination);
  for(const [other,endpoint] of clients)if(other!==key&&(!target||target===other||destination===L.BCAST||destination===0xffffffff)){router.send(packet,endpoint.port,endpoint.address);forwarded++;}
 });
 await new Promise(resolve=>router.bind(0,'127.0.0.1',resolve));
 const endpoint=publicEndpoint||{host:'127.0.0.1',port:router.address().port};
 const emu=new Emu(fixtureAddress(0x0a0d1020),'Reference peer',endpoint,GAME);await emu.start();
 const originalMessage=emu.onMsg.bind(emu);let nativeGameBytes=0;
 emu.onMsg=m=>{
  if(m[0]===1){const p=L.parseIpv4(m.subarray(1));if(p&&p.proto===6&&p.body.length>=20&&p.body.readUInt16BE(2)===6501){
   const t=p.body,key=L.Tcp.key(emu.ip,6501,p.src,t.readUInt16BE(0));let c=emu.tcps.get(key);
   if(!c&&(t[13]&2)){c=new L.Tcp(emu.ip,6501,p.src,t.readUInt16BE(0),pkt=>emu.sendIp(pkt));emu.tcps.set(key,c);c.onData=data=>{nativeGameBytes+=data.length;c.write(data);};c.accept(t.readUInt32BE(4));return;}
  }}originalMessage(m);
 };
 if(mode==='join'){
  const originalStation=emu.onStation.bind(emu);
  emu.onStation=connection=>{const write=connection.write.bind(connection);connection.write=frame=>write(Buffer.concat([frame,frame]));originalStation(connection);};
  emu.allowStationName='Native guest';emu.host();emu.hosting[0x10]=4;emu.hosting[0x1f]=8;emu.hosting.writeUInt16LE(1,0x96);}
 const child=spawn(process.argv[2],[mode,String(endpoint.port),endpoint.host],{stdio:['ignore','pipe','pipe']});let output='',errors='';let result;
 child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>errors+=b);child.on('close',(code,signal)=>result={code,signal});
 try{
  await until(()=>/READY /.test(output)||(result&&result.code!==0));assert(!result,errors||output);
  const nativeIp=Number(/READY (\d+)/.exec(output)[1]);
  if(mode==='host'){
   const connection=emu.join(nativeIp);connection.onOpen=()=>{const n=nodeInfo(emu.ip,emu.name);n.writeUInt16LE(1,46);connection.write(L.ldnFrame(L.LT.CONNECT,n));};
   await until(()=>emu.syncs?.length||(result&&result.code!==0));assert(!result,errors);assert.equal(emu.syncs.at(-1).readUInt32LE(0x68),nativeIp);
  }else{
   await until(()=>emu.stations.length||(result&&result.code!==0));
   await until(()=>output.includes('ADMITTED\n')||result);
   assert.match(output,/^ADMITTED$/m,errors||output);
  }
  const game=new L.Tcp(emu.ip,50110,nativeIp,6500,pkt=>emu.sendIp(pkt));emu.tcps.set(game.key(),game);
  const streamData=Buffer.alloc(16384);for(let i=0;i<streamData.length;i++)streamData[i]=i%239;
  let gameReply=Buffer.alloc(0);game.onOpen=()=>game.write(streamData);game.onData=data=>{gameReply=Buffer.concat([gameReply,data]);};game.connect();
  await until(()=>gameReply.length===streamData.length||(result&&result.code!==0),12000);assert(gameReply.equals(streamData),errors||output);
  await until(()=>nativeGameBytes>=28||(result&&result.code!==0));assert(nativeGameBytes>=28,errors||output);
  for(const size of[1472,4096,65507]){
   const payload=Buffer.alloc(size);for(let i=0;i<size;i++)payload[i]=i%251;
   emu.udp(nativeIp,45601,45602,payload);
   await until(()=>emu.udpIn.some(p=>p.sport===45602&&p.dport===45601&&p.data.equals(payload))||(result&&result.code!==0),12000);
   assert(emu.udpIn.some(p=>p.data.equals(payload)),`${mode} missing ${size}: ${output} ${errors}`);
  }
  await until(()=>result);assert.equal(result.code,0,errors||output);assert.match(output,/PASS control=\d+ UDP=3/);
  console.log('PASS native '+mode+' with independent standard peer, bidirectional 1472/4096/65507-byte UDP'+(publicEndpoint?' via public relay':', wire '+maxPacket+' bytes'));if(!publicEndpoint){assert(forwarded>100);assert(dropped);}console.log('PASS both game TCP origins'+(publicEndpoint?' via public relay':' and deliberate packet loss/retransmission'));
 }catch(error){const diagnosis='\nNative output: '+output+'\nNative errors: '+errors+'\nIncomplete IPv4 fragments: '+JSON.stringify([...(emu.frags||new Map())].map(([key,entry])=>({key,total:entry.total,offsets:entry.parts.map(p=>[p.off,p.body.length])})));console.error(diagnosis);throw error;}
 finally{child.kill();emu.close();router.close();}
}

async function forkRun(mode){
 const {ForkPeer,networkInfo,nodeInfo}=require('../../../server/relay/test-lanplay-fork');
 const router=dgram.createSocket('udp4'),clients=new Map(),ips=new Map();
 router.on('message',(packet,from)=>{
  const key=from.address+':'+from.port;clients.set(key,from);
  if(packet[0]===2){router.send(packet,from.port,from.address);return;}
  const offset=packet[0]===1?13:packet[0]===3?1:0;if(!offset||packet.length<offset+8)return;
  const src=packet.readUInt32BE(offset),dst=packet.readUInt32BE(offset+4);ips.set(src,key);const target=ips.get(dst);
  for(const [id,to] of clients)if(id!==key&&(!target||id===target||dst===L.BCAST||dst===0xffffffff))router.send(packet,to.port,to.address);
 });
 await new Promise(resolve=>router.bind(0,'127.0.0.1',resolve));
 const endpoint=publicEndpoint||{host:'127.0.0.1',port:router.address().port},peer=new ForkPeer(fixtureAddress(0x0a0d4433),publicEndpoint?(0xc0a80000|randomBytes(2).readUInt16LE())>>>0:0xc0a8014d,'Fork reference',endpoint);
 if(publicEndpoint)peer.start=async function(){
  this.sock=dgram.createSocket('udp4');
  this.sock.on('message',(m,from)=>{if(from.address===endpoint.host&&from.port===endpoint.port)this.receive(m);});
  await new Promise(resolve=>this.sock.bind(0,'0.0.0.0',resolve));this.raw(Buffer.from([0]));
 };
 await peer.start();if(mode==='forkjoin'){
  peer.info=networkInfo(peer.real,peer.name);peer.info[0x10]=0x91;
  peer.info.writeBigUInt64LE(GAME);peer.info.writeUInt16LE(1,0x96);peer.control(1,peer.info);
 }
 const child=spawn(process.argv[2],[mode,String(endpoint.port),endpoint.host],{stdio:['ignore','pipe','pipe']});let output='',errors='',result;
 child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>errors+=b);child.on('close',(code,signal)=>result={code,signal});
 let nativeIp,nativeBytes=0,echoBytes=Buffer.alloc(0);const streamId=333;
 const streamWire=(op,owner,id,port=0,payload=Buffer.alloc(0))=>{const b=Buffer.alloc(8+payload.length);b[0]=op;b[1]=owner;b.writeUInt16LE(id,2);b.writeUInt16LE(port,4);b.writeUInt16LE(payload.length,6);payload.copy(b,8);return b;};
 let cursor=0;
 const loop=setInterval(()=>{
  for(;cursor<peer.received.length;cursor++){
   const item=peer.received[cursor],b=item.body;if(b.length<8)continue;
   if(b[0]===2&&b.readUInt16LE(6)===b.length-8){
    if(b[1]===0){nativeBytes+=b.length-8;peer.game(nativeIp,nativeIp,streamWire(2,1,b.readUInt16LE(2),0,b.subarray(8)),11453);}
    else if(b.readUInt16LE(2)===streamId)echoBytes=Buffer.concat([echoBytes,b.subarray(8)]);
   }
  }
 },10);
 try{
  await until(()=>/READY /.test(output)||(result&&result.code!==0));assert(!result,errors||output);nativeIp=Number(/READY (\d+)/.exec(output)[1]);
  if(mode==='forkhost'){
   peer.control(0);await until(()=>peer.scan.some(s=>s.from===nativeIp)||(result&&result.code!==0));assert(!result,errors||output);
   const seen=peer.scan.find(s=>s.from===nativeIp);const n=nodeInfo(peer.real,peer.name);n.writeUInt16LE(1,46);
   peer.control(2,Buffer.concat([n,seen.info.subarray(0x20,0x26)]));await until(()=>peer.sync.length||(result&&result.code!==0));assert(!result,errors||output);
  }else{
   await until(()=>peer.connectSeen||(result&&result.code!==0));
   await until(()=>output.includes('ADMITTED\n')||result);
   assert.match(output,/^ADMITTED$/m,errors||output);
  }
  peer.game(nativeIp,nativeIp,streamWire(1,0,streamId,6500),11453);await delay(100);
  const expected=Buffer.alloc(16384);for(let i=0;i<expected.length;i++)expected[i]=i%239;
  for(let at=0;at<expected.length;at+=512){peer.game(nativeIp,nativeIp,streamWire(2,0,streamId,0,expected.subarray(at,at+512)),11453);await delay(20);}
  await until(()=>echoBytes.length===expected.length||(result&&result.code!==0));assert(echoBytes.equals(expected),errors||output);
  await until(()=>nativeBytes>=28||(result&&result.code!==0));assert(nativeBytes>=28,errors||output);
  for(const size of[64,1400,1472]){
   const data=Buffer.alloc(size,0x71);peer.game(nativeIp,nativeIp,data,45602);
   await until(()=>peer.received.some(x=>x.body.equals(data))||(result&&result.code!==0));assert(peer.received.some(x=>x.body.equals(data)),errors||output);
  }
  await until(()=>result);assert.equal(result.code,0,errors||output);
  console.log('PASS native '+mode+' with real/virtual fork addressing, both legacy stream origins and fragmented1472 UDP');
 }catch(error){error.message+='\nNative output: '+output+'\nNative errors: '+errors;throw error;}
 finally{clearInterval(loop);child.kill();peer.close();router.close();}
}

function fixtureInfo(ip, game=GAME) {
 const info=Buffer.alloc(0x480);info.writeBigUInt64LE(game);info[0x10]=3;info[0x1f]=9;
 info[0x20]=2;info.writeUInt32LE(ip,0x22);info[0x66]=8;info[0x67]=1;
 info.writeUInt32LE(ip,0x68);info[0x73]=1;info.writeUInt16LE(1,0x96);return info;
}
async function scanRun(mode) {
 const router=dgram.createSocket('udp4');let emitted=false,unicast=false;
 router.on('message',async(packet,peer)=>{
  if(packet[0]===2){router.send(packet,peer.port,peer.address);return;}
  if(packet[0]!==1)return;const ip=L.parseIpv4(packet.subarray(1));
  if(!ip||ip.proto!==17||ip.body.readUInt16BE(2)!==11452)return;
  if(ip.dst===0x0a0d7733)unicast=true;
  if(emitted)return;emitted=true;
  for(let i=0;i<97;i++){
   const source=i===96?0x0a0d7733:0x0a0d6000+i;
   const info=fixtureInfo(source,mode==='scanexact'||i===96?GAME:GAME+16n);
   const raw=L.udpPacket(source,11452,ip.src,11452,L.ldnFrame(L.LT.SCAN_RESP,info));
   router.send(Buffer.concat([Buffer.from([1]),raw]),peer.port,peer.address);await delay(3);
  }
 });
 await new Promise(resolve=>router.bind(0,'127.0.0.1',resolve));
 const child=spawn(process.argv[2],[mode,String(router.address().port)],{stdio:['ignore','pipe','pipe']});
 let output='',errors='';child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>errors+=b);
 try{const result=await new Promise(resolve=>child.on('close',(code,signal)=>resolve({code,signal})));
  assert.equal(result.code,0,errors||output);assert.match(output,/PASS scan/);if(mode==='scanexact')assert(unicast);
  console.log('PASS '+mode+' finds target after 96 unrelated advertisements');
 }finally{child.kill();router.close();}
}
async function recoveryRun(mode){
 const router=dgram.createSocket('udp4'),ports=new Set();let origin=null,firstPort=0,injected=false;
 router.on('message',(packet,peer)=>{
  firstPort ||= peer.port;ports.add(peer.port);origin=peer;tryInject();
  if(packet[0]===2&&mode!=='rebind')router.send(packet,peer.port,peer.address);
  if(packet[0]===2&&mode==='rebind'&&peer.port!==firstPort)router.send(packet,peer.port,peer.address);
 });
 await new Promise(resolve=>router.bind(0,'127.0.0.1',resolve));
 const child=spawn(process.argv[2],[mode,String(router.address().port)],{stdio:['ignore','pipe','pipe']});
 let output='',errors='';
 function tryInject(){
  const match=/READY (\d+)/.exec(output);
  if(mode==='collision'&&match&&origin&&!injected){
   injected=true;const address=Number(match[1]);
   const wire=L.udpPacket(address,11452,L.BCAST,11452,L.ldnFrame(L.LT.SCAN,Buffer.alloc(0)));
   router.send(Buffer.concat([Buffer.from([1]),wire]),origin.port,origin.address);
  }
 }
 child.stdout.on('data',b=>{output+=b;tryInject();});child.stderr.on('data',b=>errors+=b);
 try{const result=await new Promise(resolve=>child.on('close',(code,signal)=>resolve({code,signal})));
  assert.equal(result.code,0,errors||output);assert.match(output,/PASS /);
  if(mode==='rebind')assert(ports.size>1,'outer UDP endpoint did not actually change');
  console.log(output.trim().split('\n').at(-1));
 }finally{child.kill();router.close();}
}

(async()=>{
 const mode=process.env.LANPLAY_TEST_MODE;
 if(mode){assert(publicEndpoint&&['host','join','forkhost','forkjoin'].includes(mode));return mode.startsWith('fork')?forkRun(mode):run(mode);}
 if(!publicEndpoint){await scanRun('scanfilter');await scanRun('scanexact');await recoveryRun('collision');await recoveryRun('rebind');}
 await run('host');await run('join');if(publicEndpoint)await delay(45000);await forkRun('forkhost');await forkRun('forkjoin');
})().catch(error=>{console.error(error);process.exitCode=1;});
