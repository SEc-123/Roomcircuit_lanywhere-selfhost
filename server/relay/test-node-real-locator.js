'use strict';
const assert=require('node:assert/strict');
const {test}=require('node:test');
const dgram=require('node:dgram');
const fs=require('node:fs/promises');
const path=require('node:path');
const os=require('node:os');
const {spawn}=require('node:child_process');
const {once}=require('node:events');
const {setTimeout:delay}=require('node:timers/promises');
const {Peer}=require('../../deploy/smoke-relay.cjs');
const {Emu}=require('./lanplay-probe-peer');
const {ForkPeer,nodeInfo}=require('./test-lanplay-fork');
const L=require('./lanplay');
const catalog=require('./node-catalog');

async function until(fn){for(let i=0;i<100;i++){const result=await fn();if(result)return result;await delay(100);}throw Error('fixture timed out');}
async function standard(t){
 const socket=dgram.createSocket('udp4'),clients=new Map(),ips=new Map();let forwarded=0;
 socket.on('message',(bytes,remote)=>{
  clients.set(remote.address+':'+remote.port,remote);
  if(bytes[0]===2)return socket.send(bytes.subarray(0,4),remote.port,remote.address);
  if(bytes[0]!==1)return;
  const packet=L.parseIpv4(bytes.subarray(1));if(!packet)return;ips.set(packet.src,remote);
  const destinations=packet.dst===L.BCAST||packet.dst===0xffffffff?[...clients.values()]:[ips.get(packet.dst)].filter(Boolean);
  for(const destination of destinations)if(destination.port!==remote.port||destination.address!==remote.address){forwarded++;socket.send(bytes,destination.port,destination.address);}
 });
 await new Promise(resolve=>socket.bind(0,'127.0.0.1',resolve));t.after(()=>socket.close());
 return {host:'127.0.0.1',port:socket.address().port,count:()=>forwarded};
}
async function launch(t,endpoint,{fastLobby=false}={}){
 const directory=await fs.mkdtemp(path.join(os.tmpdir(),'node-locator-'));
 const childFile=path.join(directory,'realm.cjs'),mainFile=path.join(directory,'main.cjs');
 const providers=[{id:'lanplay-nl',host:endpoint.host,port:endpoint.port,allowPrivate:true}];
 const override=`require.cache[require.resolve(${JSON.stringify(path.join(__dirname,'lanplay-providers.js'))})]={exports:${JSON.stringify(providers)}};`;
 await fs.writeFile(childFile,override+`require(${JSON.stringify(path.join(__dirname,'server.js'))});`);
 const clock=fastLobby?`const u=require(${JSON.stringify(path.join(__dirname,'unified-lobby.js'))}),c=u.create;u.create=options=>c({...options,now:()=>performance.now()*20});`:'';
 await fs.writeFile(mainFile,override+clock+`const realms=require(${JSON.stringify(path.join(__dirname,'lanplay-realms.js'))}),original=realms.create;realms.create=options=>original({...options,serverFile:${JSON.stringify(childFile)},startupMs:5000});require(${JSON.stringify(path.join(__dirname,'server.js'))});`);
 const child=spawn(process.execPath,[mainFile,'--port','0'],{env:{...process.env,LANYWHERE_STATE_DIR:directory,LANYWHERE_STATS_PORT:'0',LANYWHERE_OPS_PORT:'0',LANYWHERE_PUBLIC_PORT:'0',LANYWHERE_LANPLAY_PORT:'0',LANYWHERE_SHUTDOWN_GRACE_MS:'0',LANYWHERE_ENABLE_PUBLIC_UPSTREAM:'0',LANYWHERE_ENABLE_LANPLAY_UPSTREAM:'1',LANYWHERE_LANPLAY_PROVIDERS:'lanplay-nl',LANYWHERE_ENABLE_COMMUNITY_DIRECT:'0',LANYWHERE_CONTROL_BIND_ADDRESS:'127.0.0.1',LANYWHERE_UDP_BIND_ADDRESS:'127.0.0.1'},stdio:['ignore','pipe','pipe']});
 let log='';child.stdout.on('data',bytes=>log+=bytes);child.stderr.on('data',bytes=>log+=bytes);const closed=once(child,'exit');
 const port=await until(()=>{if(child.exitCode!==null)throw Error(log);return Number(/datos UDP en (\d+)/.exec(log)?.[1]);});
 const peers=[];
 const stop=async()=>{for(const peer of peers)peer.close();if(child.exitCode===null&&child.signalCode===null)child.kill('SIGTERM');await closed;};
 t.after(async()=>{await stop();await fs.rm(directory,{recursive:true,force:true});});
 async function peer(name){const item=new Peer(name,'127.0.0.1',port);await item.connect();peers.push(item);return item;}
 return {peer,port,stop,getLog:()=>log};
}
async function directRow(browser,game){
 const request=Buffer.alloc(20);request.writeBigUInt64LE(game,8);request.writeUInt32LE(1<<2,16);
 return until(async()=>{browser.send(0x33,request);const page=await browser.frame(0xb3);assert.equal(page.length,16+page[8]*408);return page[8]?page.subarray(16,424):null;});
}

for(const flavor of ['standard','fork'])test(`real ${flavor} locator from unified directory admits a direct peer after central shutdown`,{timeout:30000},async t=>{
 const relay=await standard(t),central=await launch(t,relay);
 const game=0x0100000000abc000n;
 const host=flavor==='standard'?new Emu(0x0a0d3333,'Host',relay,game):new ForkPeer(0x0a0d4444,0xc0a80144,'ForkHost',relay);
 const guest=flavor==='standard'?new Emu(0x0a0d5555,'Guest',relay,game):new ForkPeer(0x0a0d6666,0xc0a80266,'ForkGuest',relay);
 await host.start();host.host();if(flavor==='standard')host.hosting[0x10]=9;
 await guest.start();t.after(()=>{host.close();guest.close();});
 const browser=await central.peer('Directory');browser.send(11);assert((await browser.frame(0x89)).readUInt32LE()&32768);
 browser.send(0x32);const status=await browser.frame(0xb2);assert(status.subarray(0,32).equals(catalog.hash));assert.equal(status.readUInt32LE(32),5);
 const row=await directRow(browser,game),location=row.subarray(344);
 assert.equal(row.readUInt32LE(336),2);assert.equal(row.readUInt32LE(340),1);assert.equal(location[38],flavor==='standard'?1:2);
 const actual=flavor==='standard'?host.ip:host.virt;
 assert.equal(location.readUInt32LE(40),actual);assert(location.subarray(0,32).equals((host.hosting||host.info).subarray(0,32)));
 if(flavor==='fork')assert.notEqual(location.readUInt32LE(40),host.real,'outer virtual target is preserved independently of inner Wi-Fi address');
 browser.send(0x28,Buffer.alloc(8));const legacy=await browser.frame(0xa8);assert.equal(legacy[8],1);assert.equal(legacy.subarray(12+336,12+369).toString().replace(/\0.*$/,''),'lanplay-nl');
 await central.stop();
 if(flavor==='standard'){
  guest.udp(L.BCAST,11452,11452,L.ldnFrame(0,Buffer.alloc(0)));await until(()=>guest.scanResults.some(found=>found.from===actual&&found.info.subarray(0,32).equals(location.subarray(0,32))));
  guest.join(location.readUInt32LE(40));await until(()=>guest.syncs?.length);
  guest.udp(host.ip,12345,12345,Buffer.from('direct-standard'));await until(()=>host.udpIn.some(packet=>packet.text==='direct-standard'));
  host.udp(guest.ip,12345,12345,Buffer.from('direct-return'));await until(()=>guest.udpIn.some(packet=>packet.text==='direct-return'));
 }else{
  guest.control(0);await until(()=>guest.scan.some(found=>found.from===actual&&found.info.subarray(0,32).equals(location.subarray(0,32))));
  guest.control(2,Buffer.concat([nodeInfo(guest.real,guest.name),location.subarray(32,38)]));await until(()=>guest.sync.some(info=>info[0x67]===2));
  guest.game(actual,host.real,Buffer.from('direct-fork'));await until(()=>host.received.some(packet=>packet.body.toString()==='direct-fork'));
  host.game(guest.virt,guest.real,Buffer.from('direct-return'));await until(()=>guest.received.some(packet=>packet.body.toString()==='direct-return'));
 }
 assert(relay.count()>5);
});

module.exports={standard,launch,directRow};

test('temporary measurement groups run through real relay frames without creating game rooms',{timeout:15000},async t=>{
 const relay=await standard(t),central=await launch(t,relay);
 const first=await central.peer('First'),second=await central.peer('Second');
 function request(op,code,token,median){
  const body=Buffer.alloc(240);body.writeUInt32LE(op);if(code)body.write(code,4);body.fill(token,12,28);
  if(median!==undefined){body.writeUInt32LE(1,28);body.writeUInt32LE(0,32);body.writeUInt16LE(median,36);body[40]=5;body[41]=5;body[42]=1;body[43]=1;}
  return body;
 }
 first.send(0x34,request(1,'',1));const created=await first.frame(0xb4);assert.equal(created.length,292);assert.equal(created.readUInt32LE(),0);const code=created.toString('ascii',4,10);
 second.send(0x34,request(2,code,2));assert.equal((await second.frame(0xb4))[30],2);
 first.send(0x34,request(3,code,1,30));await first.frame(0xb4);
 second.send(0x34,request(3,code,2,70));const measured=await second.frame(0xb4);assert.equal(measured[31],2);assert.equal(measured.readUInt32LE(36),70);assert.equal(measured[49],1);
 first.send(0x33,Buffer.from([...Buffer.alloc(16),0xff,0x1f,0,0]));assert.equal((await first.frame(0xb3))[8],0);
 await first.enter();first.send(0x34,request(4,code,1));const denied=await until(()=>{const index=first.frames.findIndex(frame=>frame[0]===0x85);return index>=0?first.frames.splice(index,1)[0]:null;});assert.equal(denied[1],4,'active game session cannot be reused for node selection');
 await first.leave();second.send(0x34,request(5,code,2));assert.equal((await second.frame(0xb4))[30],1);
});
