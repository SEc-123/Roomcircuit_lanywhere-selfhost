'use strict';

const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { setTimeout: delay } = require('node:timers/promises');
const registry = require('./custom-node-registry');
const catalog = require('./node-catalog');
const directory = require('./node-directory');
const locator = require('./node-locator');
const { Peer } = require('../../deploy/smoke-relay.cjs');

function fixture() {
    return { version: 1, nodes: [{id: 13, host: 'moscow.example.org', port: 11451,
        name: 'Moscow', region: 2, enabled: true}] };
}
function bad(doc) { assert.throws(() => registry.validate(doc)); }

test('administrator registry checks limits, public hosts and persistent node identities', t => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'roomcircuit-registry-'));
    t.after(() => fs.rmSync(dir, {recursive: true, force: true}));
    const file = path.join(dir, 'nodes.json');
    const put = value => fs.writeFileSync(file, JSON.stringify(value));
    const doc = fixture(); put(doc);
    assert.equal(registry.load(file, dir)[0].provider, 'custom-13');
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(dir,'custom-node-identities.json'))), {'13': 'moscow.example.org:11451'});
    put({version: 1, nodes: []}); assert.deepEqual(registry.load(file, dir), []);
    const reused = fixture(); reused.nodes[0].host = 'changed.example.org'; put(reused);
    assert.throws(() => registry.load(file, dir), /cannot be reused/);
    const moved = fixture(); moved.nodes[0].id = 14; put(moved);
    assert.throws(() => registry.load(file, dir), /another slot/);
    for (const address of ['127.0.0.1','10.1.1.1','192.168.1.1','169.254.169.254','100.64.0.1','198.18.0.5','224.0.0.1']) {
        const value=fixture();value.nodes[0].host=address;bad(value);
        assert.equal(registry.publicIPv4(address),false);
    }
    for (const value of [0,12,24,31,-1]) {const d=fixture();d.nodes[0].id=value;bad(d);}
    for (const value of ['EXAMPLE.COM','http://example.com','admin@host.example','x.localhost','bad..example']) {
        // .localhost is a syntactically valid hostname, but DNS private results
        // are rejected by the network adapter before connecting.
        if (value === 'x.localhost') continue;
        const d=fixture();d.nodes[0].host=value;bad(d);
    }
    const duplicate=fixture();duplicate.nodes.push({...duplicate.nodes[0],id:14});bad(duplicate);
    const disabled=fixture();disabled.nodes[0].enabled=false;
    const data=registry.response(registry.validate(disabled), new Map());
    assert.equal(data.length,232);assert(data.subarray(0,32).equals(catalog.hash));
    assert.equal(data[32],1);assert.equal(data.readUInt32LE(36),0);
    assert.equal(data.readUInt32LE(40),13);assert.equal(data.readUInt16LE(50),0);
    const sources = new Map([['custom-13', Object.assign([], {upstreamAddress:'127.0.0.1'})]]);
    const unsafe = registry.response(registry.validate(fixture()), sources);
    assert.equal(unsafe.readUInt32LE(36), 0);assert.deepEqual([...unsafe.subarray(228,232)], [0,0,0,0]);
});

test('extended lobby includes exact registered identity while legacy page stays limited', () => {
    const entry = registry.validate(fixture())[0];
    const summary=Buffer.alloc(336); summary.write('ABC234',0,'ascii');summary.writeBigUInt64LE(0x0100123456789000n,8);summary[32]=1;summary[33]=4;
    const info=Buffer.alloc(0x480);info.writeBigUInt64LE(0x0100123456789000n);info.fill(0x71,16,32);info.fill(0x21,32,38);
    const loc=locator.create(info,0x0a0d4321,1,'ABC234');
    const sources=new Map([['self',[]],[entry.provider,[{summary,locator:loc,upstreamAddress:'9.9.9.9'}]]]);
    const extended=directory.create({source:()=>sources,providerIds:[entry.provider],communityDirect:false,nodes:[...catalog.nodes,entry]});
    const legacy=directory.create({source:()=>sources,providerIds:[],communityDirect:false});
    const req=Buffer.alloc(20);req.writeUInt32LE(1 << 13,16);
    assert.equal(legacy.page(req),null);
    const page=extended.page(req,true);assert.equal(page[8],1);
    assert.equal(page.readUInt32LE(16+336),13);assert.equal(page.readUInt32LE(16+340),1);
    assert(page.subarray(16+344,16+344+52).equals(loc.subarray(0,52)));
    assert.deepEqual([...page.subarray(16+396,16+400)],[9,9,9,9]);
    sources.get(entry.provider)[0].upstreamAddress = '8.8.8.8';
    const newer=extended.page(req,true);assert.notEqual(page.readUInt32LE(),newer.readUInt32LE());
    assert.deepEqual([...newer.subarray(16+396,16+400)],[8,8,8,8]);
    const previous=Buffer.from(req);previous.writeUInt32LE(page.readUInt32LE(),4);
    assert.deepEqual([...extended.page(previous,true).subarray(16+396,16+400)],[9,9,9,9],
        'old snapshot retains the address of the server that discovered its room');
    extended.shutdown();legacy.shutdown();
});

test('custom room metadata stays scoped to its live server endpoint', () => {
    const { createHash } = require('node:crypto');
    const metadata = require('./direct-room-metadata').create();
    const node = registry.validate(fixture())[0];
    const proof = Buffer.alloc(32, 7), hostIp = 0x0a0d4321;
    const binding = Buffer.alloc(8); binding.writeUInt32LE(13); binding.writeUInt32LE(hostIp,4);
    const game = 0x0100123456789000n, info = Buffer.alloc(0x480);
    info.writeBigUInt64LE(game); createHash('sha256').update(proof).update(binding).digest().copy(info,16,0,16);
    info.fill(0x21,32,38);
    const location=locator.create(info,hostIp,1,'ABC234');
    const summary=Buffer.alloc(336);summary.write('ABC234');summary.writeBigUInt64LE(game,8);summary[32]=1;summary[33]=4;
    const source=new Map([[node.provider,[{summary,locator:location,upstreamAddress:'9.9.9.9'}]]]);
    const d=directory.create({source:()=>source,providerIds:[node.provider],communityDirect:false,
        nodes:[...catalog.nodes,node],metadata});
    const write=Buffer.alloc(384);write.writeUInt32LE(1);write.writeUInt32LE(13,4);write.writeUInt32LE(hostIp,8);
    location.copy(write,12,0,38);write[50]=1;write[51]=4;write.write('1234',52);proof.copy(write,76);
    write.writeBigUInt64LE(0x0100000000001000n,112);write.writeBigUInt64LE(0x0100000000001001n,120);
    write.write('Duck Game',128);
    assert.equal(d.exchangeMetadata(write,'owner').readUInt32LE(),0);
    const req=Buffer.alloc(20);req.writeUInt32LE(1<<13,16);
    const old=d.page(req,true);assert.equal(old[16+35],1);
    source.get(node.provider)[0].upstreamAddress='8.8.8.8';
    const previous=Buffer.from(req);previous.writeUInt32LE(old.readUInt32LE(),4);
    assert.equal(d.page(previous,true)[16+35],0,'stale endpoint snapshot cannot inherit metadata from the new endpoint');
    const forged=Buffer.from(write);forged[76]^=1;
    assert.equal(d.exchangeMetadata(forged,'guest').readUInt32LE(),4);
    d.shutdown();
});

test('actual relay negotiates custom catalog and V2 without breaking legacy requests', {timeout:15000}, async t => {
    const dir=fs.mkdtempSync(path.join(os.tmpdir(),'roomcircuit-custom-wire-'));
    const config=path.join(dir,'nodes.json');fs.writeFileSync(config,JSON.stringify(fixture()));
    const child=spawn(process.execPath,[path.join(__dirname,'server.js'),'--port','0'],{env:{...process.env,
        ROOMCIRCUIT_CUSTOM_NODES:config,LANYWHERE_STATE_DIR:dir,LANYWHERE_STATS_PORT:'0',LANYWHERE_PUBLIC_PORT:'0',
        LANYWHERE_OPS_PORT:'0',LANYWHERE_LANPLAY_PORT:'0',LANYWHERE_ENABLE_LANPLAY_UPSTREAM:'0',
        LANYWHERE_ENABLE_PUBLIC_UPSTREAM:'0',LANYWHERE_ENABLE_COMMUNITY_DIRECT:'0',LANYWHERE_SHUTDOWN_GRACE_MS:'0'},
        stdio:['ignore','pipe','pipe']});
    let log='';child.stdout.on('data',b=>log+=b);child.stderr.on('data',b=>log+=b);
    const exit=once(child,'exit');const peers=[];
    t.after(async()=>{for(const peer of peers)peer.close();if(child.exitCode===null&&child.signalCode===null)child.kill('SIGTERM');await exit;fs.rmSync(dir,{recursive:true,force:true});});
    let port=0;
    for(let i=0;i<100&&!port;i++){if(child.exitCode!==null)throw Error(log);port=Number(/datos UDP en (\d+)/.exec(log)?.[1]||0);if(!port)await delay(50);}
    assert(port,log);
    const peer=new Peer('Wire','127.0.0.1',port);await peer.connect();peers.push(peer);
    peer.send(11);assert((await peer.frame(0x89)).readUInt32LE() & registry.CAPABILITY);
    peer.send(0x32);const old=await peer.frame(0xb2);assert.equal(old.length,40);assert(old.subarray(0,32).equals(catalog.hash));
    peer.send(0x38);const response=await peer.frame(0xb8);assert.equal(response.length,232);assert.equal(response[32],1);
    assert.equal(response.readUInt32LE(40),13);assert.equal(response.readUInt16LE(48),11451);
    assert.equal(response.toString('ascii',52,180).split('\0')[0],'moscow.example.org');
    const request=Buffer.alloc(20);request.writeUInt32LE(1<<13,16);
    peer.send(0x33,request);await assert.rejects(peer.frame(0xb3), /protocol error 4/);
    peer.send(0x39,request);const page=await peer.frame(0xb9);assert.equal(page.length,16);assert.equal(page[8],0);
    peer.send(0x38,Buffer.from([1]));await assert.rejects(peer.frame(0xb8), /protocol error 4/);
});

test('registered custom node discovers and joins the exact room via a real LAN Play socket', {timeout: 40000}, async t => {
    const dgram = require('node:dgram');
    const L = require('./lanplay');
    const { Emu } = require('./lanplay-probe-peer');
    const relay = dgram.createSocket('udp4'), peers = new Map(), ips = new Map();
    relay.on('message', (bytes, remote) => {
        peers.set(`${remote.address}:${remote.port}`, remote);
        if (bytes[0] === 2) return relay.send(bytes.subarray(0,4),remote.port,remote.address);
        if (bytes[0] !== 1) return;
        const packet=L.parseIpv4(bytes.subarray(1)); if (!packet) return;
        ips.set(packet.src,remote);
        const targets=packet.dst===L.BCAST||packet.dst===0xffffffff?[...peers.values()]:[ips.get(packet.dst)].filter(Boolean);
        for(const target of targets)if(target.port!==remote.port||target.address!==remote.address)relay.send(bytes,target.port,target.address);
    });
    await new Promise(resolve=>relay.bind(0,'127.0.0.1',resolve));
    const endpoint={host:'127.0.0.1',port:relay.address().port};
    const dir=fs.mkdtempSync(path.join(os.tmpdir(),'roomcircuit-custom-e2e-'));
    const config=path.join(dir,'nodes.json'), doc=fixture();doc.nodes[0].port=endpoint.port;fs.writeFileSync(config,JSON.stringify(doc));
    // Test seam changes only the outbound upstream destination to an isolated
    // loopback server. Public address validation remains enabled in production.
    const shim=path.join(dir,'shim.cjs');
    const lanplayFile=path.join(__dirname,'lanplay.js'),realmsFile=path.join(__dirname,'lanplay-realms.js'),serverFile=path.join(__dirname,'server.js');
    fs.writeFileSync(shim,`const l=require(${JSON.stringify(lanplayFile)}), c=l.create;l.create=o=>c(o.upstream?{...o,onEndpoint:()=>o.onEndpoint('8.8.8.8'),upstream:{...o.upstream,host:'127.0.0.1',allowPrivate:true}}:o);`+
        `const r=require(${JSON.stringify(realmsFile)}), f=r.create;r.create=o=>f({...o,serverFile:${JSON.stringify(shim)},startupMs:5000});require(${JSON.stringify(serverFile)});`);
    const child=spawn(process.execPath,[shim,'--port','0'],{env:{...process.env,ROOMCIRCUIT_CUSTOM_NODES:config,
        LANYWHERE_STATE_DIR:dir,LANYWHERE_STATS_PORT:'0',LANYWHERE_PUBLIC_PORT:'0',LANYWHERE_OPS_PORT:'0',
        LANYWHERE_LANPLAY_PORT:'0',LANYWHERE_ENABLE_LANPLAY_UPSTREAM:'1',LANYWHERE_LANPLAY_PROVIDERS:'lanplay-nl',
        LANYWHERE_ENABLE_PUBLIC_UPSTREAM:'0',LANYWHERE_ENABLE_COMMUNITY_DIRECT:'0',LANYWHERE_SHUTDOWN_GRACE_MS:'0',
        LANYWHERE_CONTROL_BIND_ADDRESS:'127.0.0.1',LANYWHERE_UDP_BIND_ADDRESS:'127.0.0.1'},stdio:['ignore','pipe','pipe']});
    let log='';child.stdout.on('data',b=>log+=b);child.stderr.on('data',b=>log+=b);const exited=once(child,'exit');
    let browser,host,guest;
    t.after(async()=>{browser?.close();host?.close();guest?.close();if(child.exitCode===null&&child.signalCode===null)child.kill('SIGTERM');await exited;relay.close();fs.rmSync(dir,{recursive:true,force:true});});
    const wait=async fn=>{for(let i=0;i<150;i++){const value=await fn();if(value)return value;if(child.exitCode!==null)throw Error(log);await delay(100);}throw Error('fixture timed out: '+log);};
    const port=await wait(()=>Number(/datos UDP en (\d+)/.exec(log)?.[1]||0));
    const game=0x0100000000abc000n;
    host=new Emu(0x0a0d3333,'Host',endpoint,game);guest=new Emu(0x0a0d5555,'Guest',endpoint,game);
    await host.start();host.host();host.hosting[0x10]=9;await guest.start();
    browser=new Peer('Directory','127.0.0.1',port);await browser.connect();
    browser.send(0x38);const catalogReply=await browser.frame(0xb8);assert.equal(catalogReply.readUInt32LE(40),13);
    const query=Buffer.alloc(20);query.writeBigUInt64LE(game,8);query.writeUInt32LE(1<<13,16);
    const row=await wait(async()=>{browser.send(0x39,query);const page=await browser.frame(0xb9);return page[8]?page.subarray(16,424):null;});
    assert.equal(row.readUInt32LE(336),13);assert.equal(row.readUInt32LE(340),1);assert.deepEqual([...row.subarray(396,400)],[8,8,8,8]);
    browser.send(0x38);const readyCatalog=await browser.frame(0xb8);
    assert(readyCatalog.readUInt32LE(36) & (1<<13));
    assert.deepEqual([...readyCatalog.subarray(40+188,40+192)],[8,8,8,8]);
    const target=row.subarray(344);assert.equal(target.readUInt32LE(40),host.ip);
    assert(target.subarray(0,32).equals(host.hosting.subarray(0,32)));
    guest.udp(L.BCAST,11452,11452,L.ldnFrame(0,Buffer.alloc(0)));
    await wait(()=>guest.scanResults.some(found=>found.from===host.ip&&found.info.subarray(0,32).equals(target.subarray(0,32))));
    guest.join(host.ip);await wait(()=>guest.syncs?.length);
    guest.udp(host.ip,12345,12345,Buffer.from('custom-direct'));
    await wait(()=>host.udpIn.some(packet=>packet.text==='custom-direct'));
});

test('outbound gateway refuses private DNS answers before opening upstream socket', async () => {
    const dns = require('node:dns').promises;
    const original = dns.lookup;
    const lanplay = require('./lanplay');
    try {
        for (const address of ['127.0.0.1', '169.254.169.254', '198.18.0.2', '192.168.1.2']) {
            dns.lookup = async () => ({address, family: 4});
            let failure;
            const gateway=lanplay.create({port:0,upstream:{id:'custom-13',host:'dns.example.org',port:11451},
                log:()=>{},rooms:new Map(),count:()=>{},paused:()=>false,frame:()=>{},forwardData:()=>{},
                memberAt:()=>null,isOpen:()=>true,onFailure:error=>{failure=error;}});
            for(let i=0;i<30&&!failure;i++)await delay(10);
            assert.match(failure?.message || '',/non-public address/);
            gateway.close();
        }
    } finally { dns.lookup = original; }
});
