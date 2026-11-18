// SPDX-License-Identifier: GPL-2.0-only
// NX Pocket's pinned LDN code over real kernel sockets, Linux TUN and a relay process.
'use strict';
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const { once } = require('node:events');
const { randomUUID } = require('node:crypto');
const { mkdtempSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const path = require('node:path');
const { zstdDecompressSync } = require('node:zlib');
const { uint, waitFor, anyIp } = require('../../../server/eden/test-peer');
const { ryuPeer, edenPeer, yuzuPeer, freeTcpPort } = require('../../../server/eden/test-ryuldn-crossplay');
const { consolePeer, GAME, FIRST_NODE, POLL_SOURCE, game, ldn, recvLdn, nodeInfo,
    networkInfo, withStations, take, reserveUdpPort } = require('../../../server/eden/test-crossplay');
const ryu = require('../../../server/ryuldn/protocol');
const root = path.resolve(__dirname, '../../..');
const upstream = process.env.NX_POCKET_SOURCE;
const nx = uint(0x0a0dfa02, 4);
const delay = ms => new Promise(ok => setTimeout(ok, ms));

async function find(query) {
    const until = Date.now() + 12000;
    while (Date.now() < until) {
        const found = await query();
        if (found) return found;
        await delay(150);
    }
    throw Error('NX reference room was not found');
}

function runReference(box, role, lanPort, echo = false) {
    if (!upstream) throw Error('Set NX_POCKET_SOURCE to the pinned upstream checkout');
    const command = spawn('docker', ['run','--rm','--name',box,'--cap-add','NET_ADMIN',
        '--device','/dev/net/tun','--add-host','host.docker.internal:host-gateway',
        '-e','RC_LANPLAY_PORT='+lanPort,
        ...(echo ? ['-e','RC_CLIENT_IP=10.13.250.6','-e','RC_ECHO_PEER=1'] : []),
        '-v',path.join(root,'tools/lanplay-tun')+':/tools:ro','-v',path.resolve(upstream)+':/upstream:ro',
        'roomcircuit-reference-sockets:local','/tools/reference/run-nx.sh',role],
        {stdio:['ignore','pipe','pipe']});
    let output='';
    for(const stream of [command.stdout,command.stderr]) stream.on('data',d=>output+=d);
    return once(command,'close').then(([code])=>assert.equal(code,0,output));
}

async function session(family, role, driver = runReference, label = 'NX Pocket LDN') {
    const state = mkdtempSync(path.join(tmpdir(), 'rc-nx-review-'));
    const nativePort = await freeTcpPort(), ryuPort = await freeTcpPort();
    const lanPort = await reserveUdpPort(), edenPort = await reserveUdpPort();
    const relay = spawn(process.execPath, ['server/relay/server.js','--port',String(nativePort)], {
        cwd:root, env:{...process.env, ROOMCIRCUIT_EMULATOR_BIND:'127.0.0.1',
        ROOMCIRCUIT_EDEN_PORT:family==='Yuzu'?'0':String(edenPort),
        ROOMCIRCUIT_YUZU_PORT:family==='Yuzu'?String(edenPort):'0', ROOMCIRCUIT_RYULDN_PORT:String(ryuPort),
        LANYWHERE_LANPLAY_PORT:String(lanPort),LANYWHERE_STATS_PORT:'0',LANYWHERE_PUBLIC_PORT:'0',
        LANYWHERE_CONTROL_BIND_ADDRESS:'127.0.0.1', LANYWHERE_UDP_BIND_ADDRESS:'127.0.0.1',
        LANYWHERE_STATE_DIR:state,LANYWHERE_SHUTDOWN_GRACE_MS:'0'},stdio:['ignore','pipe','inherit']
    });
    const relayDone=once(relay,'close');
    let logs='';
    relay.stdout.on('data',s=>{ logs+=s; });
    const box='rc-nx-'+randomUUID(), peers=[];
    try {
        await waitFor(()=>logs.includes('datos UDP'), 'relay startup');
        const guest = role==='guest'; // NX's role in this test
        let exchange;
        if (family==='Switch') {
            const p=await consolePeer(nativePort,'Switch'); peers.push(p);
            if(guest) await p.host(1);
            exchange=async()=>{
                if (guest) {
                    const c=await p.recv(0x84,b=>b[1]===3);
                    const info=withStations(p.info,c.subarray(2));
                    p.send(7,info);p.send(4,Buffer.concat([Buffer.from([c[0],4]),info]));
                } else {
                    const code=await find(async()=>{
                        p.send(8,GAME);const list=await p.recv(0x87);
                        return list[0] && list.subarray(1,7);
                    });
                    await p.join(code);
                    p.send(4,Buffer.concat([Buffer.from([0,3]),nodeInfo(p.ip,'Switch')]));
                    await p.recv(0x84,b=>b[1]===4);
                }
                const pkt=await waitFor(()=>take(p.data,b=>b[4]===0x82),'NX UDP at Switch');
                assert.equal(pkt.readUInt32LE(5),nx.readUInt32BE());
                assert.equal(pkt.readUInt16LE(11),5511);
                assert.equal(pkt.subarray(13).toString(),'nx-game');
                p.datagram(nx,Buffer.from('peer-ok'));
            };
        } else if(family==='Ryu') {
            const p=await ryuPeer(ryuPort,'RyuTest',peers);
            if(guest) await p.host();
            exchange=async()=>{
                if(guest) await p.wait(ryu.TYPE.SYNC,b=>b[0x67]===2);
                else {p.join(await find(async()=>(await p.rooms())[0]));await p.membership();}
                const pkt=await p.wait(ryu.TYPE.PROXY_DATA);
                assert.equal(pkt.subarray(ryu.SIZE.PROXY_DATA).toString(),'nx-game');
                const hdr=ryu.encodeProxyInfo({src:p.ip.readUInt32BE(),dst:nx.readUInt32BE(),sport:5511,dport:7788,protocol:17},ryu.SIZE.PROXY_DATA);
                hdr.writeUInt32LE(7,ryu.SIZE.PROXY_INFO);
                p.send(ryu.TYPE.PROXY_DATA,Buffer.concat([hdr,Buffer.from('peer-ok')]));
            };
        } else if (family === 'NX' || family === 'same-driver') {
            const second = family === 'NX' ? runReference : driver;
            exchange=()=>second(box+'-echo',guest ? 'host' : 'guest',lanPort,true);
        } else {
            const enetPeer=family==='Yuzu'?yuzuPeer:edenPeer;
            const p=await enetPeer(edenPort,'EmulatorTest',peers);
            const info=networkInfo(nodeInfo(p.ip,'EdenTest'),1);
            let polling;
            if(guest) {
                await recvLdn(p,0);ldn(p,1,POLL_SOURCE,info);
                polling=setInterval(()=>{if(take(p.messages,b=>b[0]===6&&b[1]===0)) ldn(p,1,POLL_SOURCE,info);},100);
            }
            exchange=async()=>{
                try {
                    if(guest) {
                        const conn=await recvLdn(p,2,nx);
                        const sync=withStations(info,conn.subarray(15));
                        ldn(p,3,nx,sync);
                    } else {
                        await find(async()=>{
                            ldn(p,0,anyIp,Buffer.alloc(0),1); await delay(170);
                            return take(p.messages,b=>b[0]===6&&b[1]===1&&b.subarray(2,6).equals(nx));
                        });
                        ldn(p,2,nx,nodeInfo(p.ip,'EdenTest')); await recvLdn(p,3,nx);
                    }
                    const pkt=await waitFor(()=>take(p.messages,b=>b[0]===5),'NX UDP at Eden');
                    assert.equal(zstdDecompressSync(pkt.subarray(21)).toString(),'nx-game');
                    game(p,nx,Buffer.from('peer-ok'));
                } finally {clearInterval(polling);}
            };
        }
        try { await Promise.all([exchange(),driver(box,role,lanPort)]); }
        catch(e) { console.error(logs.slice(-1500));throw e; }
        console.log('PASS '+label+' '+role+' ↔ '+family+' (kernel TCP join and game UDP both ways)');
    } finally {
        for(const peer of peers) await peer.close();
        spawnSync('docker',['rm','-f',box,box+'-echo'],{stdio:'ignore'});
        relay.kill('SIGTERM'); await relayDone; rmSync(state,{recursive:true,force:true});
    }
}
module.exports = { session };
if (require.main === module) (async()=>{
    for(const family of ['Switch','Ryu','Eden','Yuzu','NX'])
        for(const role of ['guest','host']) await session(family,role);
})().catch(e=>{console.error(e);process.exitCode=1;});
