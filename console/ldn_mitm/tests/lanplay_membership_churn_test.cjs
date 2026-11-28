'use strict';
const assert = require('node:assert/strict');
const {Native, router, until, delay, hash, payload} = require('./lanplay_multi_test.cjs');
const L = require('../../../server/relay/lanplay');
const {Emu} = require('../../../server/relay/lanplay-probe-peer');
const {ForkPeer, nodeInfo} = require('../../../server/relay/test-lanplay-fork');
const GAME = 0x0100123456789000n;

function stream(op, id, port = 0, data = Buffer.alloc(0)) {
    const b = Buffer.alloc(8 + data.length);
    b[0] = op;
    b.writeUInt16LE(id, 2);
    b.writeUInt16LE(port, 4);
    b.writeUInt16LE(data.length, 6);
    data.copy(b, 8);
    return b;
}

async function run(fork) {
    const relay = await router(), endpoint = {host:'127.0.0.1', port:relay.port};
    const host = fork ? new ForkPeer(0x0a0d6000, 0xc0a86000, 'Host', endpoint) :
        new Emu(0x0a0d6000, 'Host', endpoint, GAME);
    const native = new Native(relay.port, 0, process.argv[2]);
    const peers = [], live = [];
    let timer, room;
    function sync() {
        if (fork) host.control(3, room);
        else for (const c of host.connections.values()) c.write(L.ldnFrame(3, room));
    }
    function heartbeat(p) {
        const b = Buffer.alloc(12);
        room.copy(b, 0, 0x20, 0x26);
        b.writeUInt32LE(p.real, 8);
        p.control(4, b);
    }
    async function add(index) {
        const virt = 0x0a0d6000 + index;
        const p = fork ? new ForkPeer(virt, 0xc0a86000 + index, 'Peer ' + index, endpoint) :
            new Emu(virt, 'Peer ' + index, endpoint, GAME);
        peers.push(p); live.push(p); p.virtual = virt; p.gameIn = [];
        await p.start();
        if (fork) {
            p.control(2, Buffer.concat([nodeInfo(p.real, p.name), room.subarray(0x20, 0x26)]));
            await until(() => Array.from({length:8}, (_, i) => room.readUInt32LE(0x68 + i * 64)).includes(p.real),
                'reference fork admission');
            heartbeat(p);
        } else {
            p.connection = p.join(host.ip);
            await until(() => host.connections.has(p.ip), 'reference standard admission');
            const original = p.onMsg.bind(p);
            p.onMsg = wire => {
                if (wire[0] === 1) {
                    const packet = L.parseIpv4(wire.subarray(1));
                    if (packet?.proto === 6 && packet.body.length >= 20 && packet.body.readUInt16BE(2) === 6500) {
                        const t = packet.body, key = L.Tcp.key(p.ip, 6500, packet.src, t.readUInt16BE(0));
                        if (!p.tcps.has(key) && (t[13] & 2)) {
                            const c = new L.Tcp(p.ip, 6500, packet.src, t.readUInt16BE(0), b => p.sendIp(b));
                            p.tcps.set(key, c);
                            c.onData = data => p.gameIn.push(Buffer.from(data));
                            c.accept(t.readUInt32BE(4));
                            return;
                        }
                    }
                }
                original(wire);
            };
        }
        return p;
    }
    async function exchange(p, id) {
        const start = native.events.length, incoming = payload(p.virtual, id, 512);
        native.cmd(`UDP ${p.virtual} ${id} 64`);
        if (fork) p.game(native.ip, native.ip, incoming, 45000);
        else p.udp(native.ip, 46000 + id, 45000, incoming);
        await native.wait(() => native.events.slice(start).some(e => e.event === 'udp' &&
            e.peer === p.virtual && e.size === 512 && e.hash === hash(incoming)), 'peer UDP to native');
        await until(() => fork ? p.received.some(e => e.dst === p.real && e.body.equals(payload(native.ip, id, 64))) :
            p.udpIn.some(e => e.data.equals(payload(native.ip, id, 64))), 'native UDP to peer');
        native.cmd(`OPEN ${p.virtual} ${id}`);
        await native.wait(() => native.events.slice(start).some(e => e.event === 'stream' &&
            [2,7].includes(e.op) && e.id === id), 'native stream open');
        if (fork) {
            p.game(native.ip, native.ip, stream(1, 2000 + id, 6500), 11453);
            await native.wait(() => native.events.slice(start).some(e => e.event === 'stream' &&
                e.op === 1 && e.peer === p.virtual), 'peer stream open');
            p.game(native.ip, native.ip, stream(2, 2000 + id, 0, incoming), 11453);
        } else {
            const c = new L.Tcp(p.ip, 52000 + id, native.ip, 6500, b => p.sendIp(b));
            p.tcps.set(c.key(), c);
            c.onOpen = () => c.write(incoming);
            c.connect();
        }
        native.cmd(`DATA ${p.virtual} ${id} 512`);
        await native.wait(() => native.events.slice(start).some(e => e.event === 'stream' && e.op === 3 &&
            e.peer === p.virtual && e.size === 512 && e.hash === hash(incoming)), 'peer TCP to native');
        await until(() => fork ? p.received.some(e => e.dst === p.real && e.body[0] === 2 &&
            e.body.subarray(8).equals(payload(native.ip, id, 512))) :
            p.gameIn.some(b => b.equals(payload(native.ip, id, 512))), 'native TCP to peer');
    }
    try {
        await host.start();
        if (fork) {
            room = host.info = Buffer.alloc(0x480);
            room.writeBigUInt64LE(GAME); room[0x20] = 2; room.writeUInt32LE(host.virt, 0x22);
            room[0x66] = 8; room[0x67] = 1; nodeInfo(host.real, 'Host').copy(room, 0x68);
        } else {
            host.host(); room = host.hosting; host.connections = new Map();
            host.onStation = c => {
                const state = {}; let ip = 0, slot = -1;
                c.onData = data => {
                    for (const f of L.ldnRead(state, data)) if (f.type === 2) {
                        ip = f.data.readUInt32LE(0); slot = 1;
                        while (slot < 8 && room[0x68 + slot * 64 + 11]) slot++;
                        assert(slot < 8); f.data.copy(room, 0x68 + slot * 64);
                        room[0x68 + slot * 64 + 10] = slot; room[0x67]++;
                        host.connections.set(ip, c); sync();
                    }
                };
                c.onClose = () => {
                    if (slot < 0) return;
                    room.fill(0, 0x68 + slot * 64, 0x68 + (slot + 1) * 64);
                    room[0x67]--; host.connections.delete(ip); slot = -1; sync();
                };
            };
        }
        room[0x10] = 3; room[0x1f] = 9; room.writeUInt16LE(1, 0x96);
        if (fork) timer = setInterval(() => {
            host.control(1, room); sync(); for (const p of live) heartbeat(p);
        }, 60);
        for (let i = 1; i <= 6; i++) await add(i);
        await native.ready();
        native.cmd(`JOIN 0x${GAME.toString(16)} ${fork ? host.virt : host.ip} ${fork ? 2 : 1}`);
        await native.wait(() => native.last('members')?.count === 8, 'initial eight members');
        await exchange(peers[0], 50); await exchange(peers[1], 51);
        for (let round = 0; round < 2; round++) {
            const leaving = peers[round]; live.splice(live.indexOf(leaving), 1);
            if (fork) {
                const slot = Array.from({length:8}, (_, i) => room.readUInt32LE(0x68 + i * 64)).indexOf(leaving.real);
                assert(slot > 0); room.fill(0, 0x68 + slot * 64, 0x68 + (slot + 1) * 64); room[0x67]--; sync();
            } else leaving.connection.close();
            await native.wait(() => native.last('members')?.count === 7, 'authoritative member departure');
            const replacement = await add(20 + round);
            await native.wait(() => native.last('members')?.count === 8 &&
                native.last('members').ips.includes(replacement.virtual), 'replacement member');
            await exchange(replacement, 60 + round * 2);
            await exchange(peers[5], 61 + round * 2);
        }
        await native.stop();
        console.log(`PASS 8-node ${fork ? 'real-address fork' : 'standard'} room: two consecutive replacements, remaining/new peer UDP and both TCP origins`);
    } catch (error) {
        error.message += '\n' + JSON.stringify(native.report()); throw error;
    } finally {
        clearInterval(timer);
        for (const p of peers) p.close(); host.close();
        if (!native.result) native.p.kill(); relay.sock.close();
    }
}
(async () => { await run(false); await run(true); })().catch(error => {
    console.error(error); process.exitCode = 1;
});
