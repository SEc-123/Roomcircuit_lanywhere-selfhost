'use strict';
const assert=require('node:assert/strict'),dgram=require('node:dgram'),{spawn}=require('node:child_process'),fs=require('node:fs'),path=require('node:path');
const {setTimeout:delay}=require('node:timers/promises');
async function until(fn,label,ms=10000){const end=Date.now()+ms;while(Date.now()<end){if(fn())return;await delay(5);}throw Error('Timeout '+label);}
function hash(buf){let h=2166136261;for(const b of buf)h=Math.imul(h^b,16777619)>>>0;return h;}
function payload(ip,id,size){return Buffer.from(Array.from({length:size},(_,i)=>(ip+i*17+id*31)%251));}
class Native {
 constructor(port,index,driver){this.index=index;this.events=[];this.stderr='';this.stdout='';this.partial='';this.p=spawn(driver,[String(port)],{stdio:['pipe','pipe','pipe']});this.p.stdout.on('data',b=>{this.stdout+=b;this.partial+=b;for(;;){const i=this.partial.indexOf('\n');if(i<0)break;const line=this.partial.slice(0,i);this.partial=this.partial.slice(i+1);try{this.events.push(JSON.parse(line));}catch{this.stderr+='Unparsed '+line+'\n';}}});this.p.stderr.on('data',b=>this.stderr+=b);this.p.on('close',(code,signal)=>this.result={code,signal});}
 cmd(s){assert(!this.result,JSON.stringify(this.report()));this.p.stdin.write(s+'\n');}
 last(event){return this.events.filter(e=>e.event===event).at(-1);}
 async ready(){await until(()=>this.last('ready')||this.result,'native ready');assert(!this.result,JSON.stringify(this.report()));this.ip=this.last('ready').ip;}
 async wait(fn,label,ms){await until(()=>fn()||this.result,label,ms);assert(!this.result,JSON.stringify(this.report()));assert(fn(),label);}
 async stop(){if(this.result)return;this.cmd('QUIT');await until(()=>this.result,'shutdown',3000);assert.equal(this.result.code,0,JSON.stringify(this.report()));}
 report(){return {index:this.index,ip:this.ip,pid:this.p.pid,result:this.result,events:this.events,stderr:this.stderr};}
}
async function router(){const sock=dgram.createSocket('udp4'),clients=new Map(),ips=new Map(),stats={received:0,forwarded:0,maxWire:0,outerKinds:{},gameWire:{}};sock.on('message',(p,from)=>{stats.received++;stats.maxWire=Math.max(stats.maxWire,p.length);stats.outerKinds[p[0]]=(stats.outerKinds[p[0]]||0)+1;let inner=p[0]===1?p.subarray(1):p[0]===3&&p[11]===0?p.subarray(17):null;if(inner&&inner.length>=28&&inner[9]===17&&(inner.readUInt16BE(22)===45000||inner.readUInt16BE(22)===11453)){const edge=inner.readUInt32BE(12)+'-'+inner.readUInt32BE(16)+'-'+inner.readUInt16BE(22),marker=inner.readUInt16BE(4);(stats.gameWire[edge]||=[]).push(marker);}const key=from.address+':'+from.port;clients.set(key,from);if(p[0]===2){sock.send(p,from.port,from.address);return;}let off=p[0]===1?13:p[0]===3?1:0;if(!off||p.length<off+8)return;const src=p.readUInt32BE(off),dst=p.readUInt32BE(off+4);ips.set(src,key);const target=ips.get(dst);for(const[k,to]of clients)if(k!==key&&(!target||target===k||dst===0x0a0dffff||dst===0xffffffff)){sock.send(p,to.port,to.address);stats.forwarded++;}});await new Promise(r=>sock.bind(0,'127.0.0.1',r));return{sock,port:sock.address().port,stats};}
async function membership(nodes){const ips=nodes.map(n=>n.ip).sort((a,b)=>a-b);await Promise.all(nodes.map(n=>n.wait(()=>{const s=n.last('members');return s&&s.count===nodes.length&&JSON.stringify(s.ips.slice().sort((a,b)=>a-b))===JSON.stringify(ips);},'whole membership '+n.index)));}
async function traffic(nodes,tag){const incoming=new Map(nodes.map(n=>[n,n.events.length]));const sends=[];for(let i=0;i<nodes.length;i++)for(let j=0;j<nodes.length;j++)if(i!==j){const id=1+i*8+j;nodes[i].cmd(`UDP ${nodes[j].ip} ${id} 1472`);sends.push({from:nodes[i],to:nodes[j],id});}
 for(const s of sends)await s.to.wait(()=>s.to.events.slice(incoming.get(s.to)).some(e=>e.event==='udp'&&e.peer===s.from.ip&&e.id===s.id&&e.size===1472&&e.hash===hash(payload(s.from.ip,s.id,1472))),'all-pairs UDP '+tag);
 await delay(40);for(const s of sends)assert.equal(s.to.events.slice(incoming.get(s.to)).filter(e=>e.event==='udp'&&e.peer===s.from.ip&&e.id===s.id).length,1,'unicast exactly once');
 const before=new Map(nodes.map(n=>[n,n.events.length]));for(let i=0;i<nodes.length;i++)nodes[i].cmd(`UDP 4294967295 ${200+i} 64`);
 for(let i=0;i<nodes.length;i++)for(let j=0;j<nodes.length;j++)if(i!==j)await nodes[j].wait(()=>nodes[j].events.slice(before.get(nodes[j])).some(e=>e.event==='udp'&&e.peer===nodes[i].ip&&e.id===200+i&&e.hash===hash(payload(nodes[i].ip,200+i,64))),'broadcast receive');
 await delay(40);for(let i=0;i<nodes.length;i++)for(let j=0;j<nodes.length;j++)assert.equal(nodes[j].events.slice(before.get(nodes[j])).filter(e=>e.event==='udp'&&e.peer===nodes[i].ip&&e.id===200+i).length,i===j?0:1,'broadcast every other node exactly once');
 const offset=new Map(nodes.map(n=>[n,n.events.length]));for(let i=0;i<nodes.length;i++)nodes[i].cmd(`OPEN ${nodes[(i+1)%nodes.length].ip} ${300+i}`);
 for(let i=0;i<nodes.length;i++)await nodes[i].wait(()=>nodes[i].events.slice(offset.get(nodes[i])).some(e=>e.event==='stream'&&[2,7].includes(e.op)&&e.id===300+i),'TCP ring open');
 for(let i=0;i<nodes.length;i++)nodes[i].cmd(`DATA ${nodes[(i+1)%nodes.length].ip} ${300+i} 1024`);
 for(let i=0;i<nodes.length;i++){const to=nodes[(i+1)%nodes.length];await to.wait(()=>to.events.slice(offset.get(to)).some(e=>e.event==='stream'&&e.op===3&&e.peer===nodes[i].ip&&e.size===1024&&e.hash===hash(payload(nodes[i].ip,300+i,1024))),'TCP ring data');}
 return {unicastDatagrams:sends.length,broadcastDeliveries:nodes.length*(nodes.length-1),simultaneousTcpStreams:nodes.length,guestToGuestTcpStreams:nodes.length-2};
}
async function runScenario(game,count,flavor,deep=false,driver=path.join(__dirname,'multi-driver')){
 const start=Date.now(),relay=await router(),all=[];let nodes=[],proof={game,nodeCount:count,flavor,deep,hardware_or_games_executed:false,nativeProcesses:0,checks:[]};
 async function make(){const n=new Native(relay.port,all.length,driver);all.push(n);await n.ready();assert(!all.some(x=>x!==n&&x.ip===n.ip),'random native address collision');return n;}
 async function admit(n,host){n.cmd(`JOIN ${game} ${host.ip} ${flavor==='fork'?2:1}`);await n.wait(()=>n.last('join'),'join result');assert(n.last('join').ok,JSON.stringify(n.report()));}
 try{nodes=await Promise.all(Array.from({length:count},()=>make()));const host=nodes[0];host.cmd(`HOST ${game} ${count}`);await host.wait(()=>host.last('host'),'host');await Promise.all(nodes.slice(1).map(n=>admit(n,host)));await membership(nodes);proof.checks.push('complete-membership-all-nodes');proof.traffic=await traffic(nodes,'initial');for(const from of nodes)for(const to of nodes)if(from!==to){const markers=relay.stats.gameWire[`${from.ip}-${to.ip}-45000`];assert(markers?.length,'every UDP edge observed on wire');assert(markers.every(m=>flavor==='fork'?m===0x474e:m!==0x474e),'selected protocol framing on every UDP edge: '+from.ip+' -> '+to.ip);}for(let i=0;i<nodes.length;i++)if(flavor==='fork')assert(relay.stats.gameWire[`${nodes[i].ip}-${nodes[(i+1)%nodes.length].ip}-11453`]?.every(m=>m===0x474e),'fork TCP ring must use legacy stream wire');proof.checks.push('selected-protocol-wire-format-every-edge');proof.checks.push('all-pairs-udp','all-node-broadcast-exactly-once','concurrent-tcp-ring-including-guest-to-guest');
 if(deep){const excess=await make();excess.cmd(`JOIN ${game} ${host.ip} ${flavor==='fork'?2:1}`);await excess.wait(()=>excess.last('join'),'full join completion',12000);await delay(100);excess.cmd('STATE');await excess.wait(()=>excess.last('state'),'full state');excess.cmd(`UDP ${host.ip} 599 64`);await excess.wait(()=>excess.last('sentudp')?.id===599,'full candidate send result');assert.equal(excess.last('sentudp').ok,false,'full candidate must not be admitted to send');assert(!excess.last('members')?.ips.includes(excess.ip),'full candidate absent from published membership');await membership(nodes);excess.cmd('LEAVE');proof.checks.push('full-room-no-admission');
 const middle=nodes[Math.floor(count/2)];middle.cmd('LEAVE');await middle.wait(()=>middle.last('left'),'middle leave');nodes=nodes.filter(n=>n!==middle);await membership(nodes);proof.checks.push('middle-member-leave');const replacement=await make();await admit(replacement,host);nodes.push(replacement);await membership(nodes);proof.checks.push('replacement-restores-membership');proof.restoredUdp=[];for(const guest of nodes.slice(1)){const before=guest.events.length;host.cmd(`UDP ${guest.ip} 500 64`);await guest.wait(()=>guest.events.slice(before).some(e=>e.event==='udp'&&e.peer===host.ip&&e.id===500),'replacement traffic');proof.restoredUdp.push(guest.ip);}
 const epoch=host.last('host').epoch;host.cmd('LEAVE');await host.wait(()=>host.last('left'),'host leave');for(const n of nodes.slice(1))await n.wait(()=>n.events.some(e=>e.event==='disconnected'&&!e.active),'host exit propagation');proof.checks.push('host-exit-deactivates-all-guests');for(const n of nodes.slice(1)){n.cmd('LEAVE');await n.wait(()=>n.last('left'),'guest cleanup');}
 host.cmd(`HOST ${game} ${count}`);await host.wait(()=>host.last('host')?.epoch!==epoch,'host reopen');await Promise.all(nodes.slice(1).map(async n=>{const before=n.events.length;n.cmd(`JOIN ${game} ${host.ip} ${flavor==='fork'?2:1}`);await n.wait(()=>n.events.slice(before).some(e=>e.event==='join'&&e.ok),'rejoin');await n.wait(()=>n.events.slice(before).some(e=>e.event==='members'&&e.count===count&&e.ips.includes(n.ip)),'fresh rejoin membership');}));await membership(nodes);proof.checks.push('same-process-cleanup-reopen-rejoin');
 }
 for(const n of nodes)n.cmd('LEAVE');await delay(50);for(const n of all)await n.stop();proof.passed=true;
 }catch(e){proof.passed=false;proof.error=e.stack;}
 finally{proof.nativeProcesses=all.length;proof.seconds=(Date.now()-start)/1000;proof.router=relay.stats;proof.nodes=all.map(n=>n.report());for(const n of all)if(!n.result)n.p.kill('SIGKILL');relay.sock.close();}
 return proof;
}
if(require.main===module)(async()=>{
 const driver=process.argv[2];assert(driver,'built native driver is required');
 for(const flavor of ['standard','fork'])for(const count of [4,8]){
  const r=await runScenario('0x0100123456789000',count,flavor,true,driver);
  assert(r.passed,JSON.stringify(r));
  console.log(`PASS ${count} independent native LAN Play ${flavor} nodes: membership, all-pairs UDP, selected wire format, guest TCP, broadcast, full/replace/exit/reopen`);
 }
})().catch(e=>{console.error(e);process.exitCode=1;});
module.exports={runScenario,Native,router,until,delay,hash,payload,membership,traffic};
