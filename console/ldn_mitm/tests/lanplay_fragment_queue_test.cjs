'use strict';
const assert = require('node:assert/strict'), dgram = require('node:dgram');
const {spawn} = require('node:child_process');
const {setTimeout: delay} = require('node:timers/promises');
const L = require('../../../server/relay/lanplay');
const {Emu, nodeInfo} = require('../../../server/relay/lanplay-probe-peer');
async function until(check) { for(let i=0;i<1000;i++){if(check())return;await delay(10);}throw Error('fragment queue timed out'); }
(async () => {
    const router = dgram.createSocket('udp4'), peers = new Map(), addresses = new Map();
    const queued = []; let dropped = 0, peak = 0, nativeFragments = 0;
    const drain = setInterval(() => {
        const item = queued.shift();
        if (item) router.send(item.bytes, item.remote.port, item.remote.address);
    }, 2);
    router.on('message', (bytes, remote) => {
        const key = `${remote.address}:${remote.port}`; peers.set(key, remote);
        if (bytes[0] === 2) { router.send(bytes, remote.port, remote.address); return; }
        if (bytes[0] !== 1) return;
        const ip = L.parseIpv4(bytes.subarray(1)); if (!ip) return;
        addresses.set(ip.src, key); const target = addresses.get(ip.dst);
        for (const [id, to] of peers) {
            if (id === key || (target && id !== target && ip.dst !== L.BCAST)) continue;
            if (ip.proto === 17 && (ip.off || ip.mf)) {
                if (ip.src !== 0x0a0d1234) nativeFragments++;
                if (queued.length >= 10) { dropped++; continue; }
                queued.push({bytes: Buffer.from(bytes), remote: to}); peak = Math.max(peak, queued.length);
            } else router.send(bytes, to.port, to.address);
        }
    });
    await new Promise(resolve => router.bind(0, '127.0.0.1', resolve));
    const emu = new Emu(0x0a0d1234, 'Small queue peer', {host:'127.0.0.1',port:router.address().port}, 0x0100123456789000n);
    await emu.start();
    const child = spawn(process.argv[2], [String(router.address().port)]);
    let output = '', errors = '', result;
    child.stdout.on('data', b => output += b); child.stderr.on('data', b => errors += b);
    child.on('close', (code, signal) => result = {code, signal});
    try {
        await until(() => /READY /.test(output) || result); assert(!result, errors || output);
        const native = Number(/READY (\d+)/.exec(output)[1]);
        const connection = emu.join(native);
        connection.onOpen = () => { const node = nodeInfo(emu.ip, emu.name);node.writeUInt16LE(1,46);connection.write(L.ldnFrame(L.LT.CONNECT,node)); };
        await until(() => emu.syncs?.length || result); assert(!result, errors || output);
        const payload = Buffer.alloc(65507); for(let i=0;i<payload.length;i++)payload[i]=i%251;
        emu.udp(native,45601,45602,payload);
        await until(() => emu.udpIn.some(p=>p.data.equals(payload)));
        await until(() => result); assert.equal(result.code,0,errors||output);
        assert(nativeFragments>50);assert.equal(dropped,0);assert(peak<=10);
        console.log(`PASS 65507-byte UDP round trip through capacity10 relay: ${nativeFragments} native fragments, peak ${peak}, no drops`);
    } finally { child.kill();emu.close();clearInterval(drain);router.close(); }
})().catch(error => {console.error(error);process.exitCode=1;});
