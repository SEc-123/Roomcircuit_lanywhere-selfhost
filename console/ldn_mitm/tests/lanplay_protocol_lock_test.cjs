'use strict';
const assert = require('node:assert/strict');
const dgram = require('node:dgram');
const {spawn} = require('node:child_process');
const {setTimeout: delay} = require('node:timers/promises');
const L = require('../../../server/relay/lanplay');
const {Emu, nodeInfo} = require('../../../server/relay/lanplay-probe-peer');
const {ForkPeer} = require('../../../server/relay/test-lanplay-fork');
const game = 0x0100123456789000n;
async function until(check, timeout = 7000) {
    const end = Date.now() + timeout;
    while (Date.now() < end) { if (check()) return; await delay(10); }
    throw Error('protocol lock fixture timed out');
}
async function run(first) {
    const router = dgram.createSocket('udp4');
    const peers = new Map(), addresses = new Map(), nativePorts = new Set();
    let nativeIp = 0, originalNativePort = 0;
    router.on('message', (bytes, from) => {
        const key = `${from.address}:${from.port}`;
        peers.set(key, from);
        if (bytes[0] === 2) {
            if (first !== 'standard' || from.port !== originalNativePort)
                router.send(bytes, from.port, from.address);
            return;
        }
        const offset = bytes[0] === 1 ? 13 : bytes[0] === 3 ? 1 : 0;
        if (!offset || bytes.length < offset + 8) return;
        const source = bytes.readUInt32BE(offset), target = bytes.readUInt32BE(offset + 4);
        addresses.set(source, key);
        if (source === nativeIp) { nativePorts.add(from.port); originalNativePort ||= from.port; }
        const destination = addresses.get(target);
        for (const [id, to] of peers)
            if (id !== key && (!destination || id === destination || target === L.BCAST))
                router.send(bytes, to.port, to.address);
    });
    await new Promise(resolve => router.bind(0, '127.0.0.1', resolve));
    const endpoint = {host: '127.0.0.1', port: router.address().port};
    const standard = new Emu(0x0a0d1234, 'Standard', endpoint, game);
    const fork = new ForkPeer(0x0a0d2234, 0xc0a80117, 'Fork', endpoint);
    await standard.start(); await fork.start();
    const child = spawn(process.argv[2], [first, String(endpoint.port)]);
    let output = '', errors = '', result;
    child.stdout.on('data', b => output += b);
    child.stderr.on('data', b => errors += b);
    child.on('close', (code, signal) => result = {code, signal});
    let heartbeat;
    const joinStandard = (port = 50001) => {
        const connection = standard.join(nativeIp, port);
        connection.onOpen = () => {
            const node = nodeInfo(standard.ip, standard.name);
            node.writeUInt16LE(1, 46);
            connection.write(L.ldnFrame(L.LT.CONNECT, node));
        };
    };
    const joinFork = () => {
        const node = nodeInfo(fork.real, fork.name); node.writeUInt16LE(1, 46);
        const bssid = Buffer.alloc(6); bssid[0] = 2; bssid.writeUInt32LE(nativeIp, 2);
        fork.control(2, Buffer.concat([node, bssid]));
    };
    try {
        await until(() => /READY /.test(output) || result);
        assert(!result, errors || output); nativeIp = Number(/READY (\d+)/.exec(output)[1]);
        heartbeat = setInterval(() => {
            const body = Buffer.alloc(12); body[0] = 2;
            body.writeUInt32LE(nativeIp, 2); body.writeUInt32LE(fork.real, 8);
            fork.control(4, body);
        }, 1000);
        if (first === 'standard') joinStandard(); else joinFork();
        await until(() => /ACCEPT 0/.test(output) || result); assert(!result, errors || output);
        if (first === 'standard') joinFork(); else joinStandard();
        if (first === 'standard') {
            await delay(21000); joinFork();
        }
        await until(() => /RESET /.test(output) || result); assert(!result, errors || output);
        nativeIp = Number(/RESET (\d+)/.exec(output)[1]);
        if (first === 'standard') joinFork(); else joinStandard(50002);
        await until(() => result);
        assert.equal(result.code, 0, errors || output);
        if (first === 'standard') assert(nativePorts.size > 1, 'no actual socket rebind');
        console.log(`PASS ${first}-first room rejects mixed legacy traffic, rebuild accepts other flavor`);
    } finally {
        clearInterval(heartbeat); child.kill(); standard.close(); fork.close(); router.close();
    }
}
(async () => { await run('standard'); await run('fork'); })().catch(error => {
    console.error(error); process.exitCode = 1;
});
