// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const assert = require('node:assert/strict');
const { once } = require('node:events');
const dgram = require('node:dgram');
const fs = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { setTimeout: sleep } = require('node:timers/promises');
const { test } = require('node:test');
const { BCAST, FIRST_NODE, NODE_MAX, NODE_COUNT, NODE_SIZE } = require('./emulator-bridge');
const Lan = require('./lanplay');
const Ryu = require('../ryuldn/protocol');
const Stream = require('./tcp-stream-protocol');

const GAME = 0x0100000000abc000n;
const MAGIC = 0x594e414c;
const LDN = { CONNECT: 3, SYNC: 4 };
const T = { CREATE: 1, JOIN: 2, LEAVE: 3, RELAY: 4, PUBLISH: 7, FIND: 8, ROOM: 0x81, CONTROL: 0x84, GROUPS: 0x87 };
const NAME = Buffer.from('Player');

async function until(pred, message) {
    const deadline = Date.now() + 4000;
    while (Date.now() < deadline) {
        const selected = pred();
        if (selected !== undefined && selected !== false) return selected;
        await sleep(10);
    }
    throw new Error(message);
}

function take(queue, predicate) {
    const index = queue.findIndex(predicate);
    if (index >= 0) return queue.splice(index, 1)[0];
}

async function freeTcpPort() {
    const listener = net.createServer().listen(0, '127.0.0.1');
    await once(listener, 'listening');
    const { port } = listener.address();
    await new Promise(resolve => listener.close(resolve));
    return port;
}

function nickname(name) {
    const out = Buffer.alloc(32);
    out.write(name);
    return out;
}

function networkInfo(host, marker) {
    const info = Buffer.alloc(0x480);
    info.writeBigUInt64LE(GAME);
    info[0x10] = marker;
    info[0x20] = 2;
    info.writeUInt32LE(host, 0x22);
    info[NODE_MAX] = 4;
    info[NODE_COUNT] = 1;
    Ryu.nodeInfo({ ip: host, mac: info.subarray(0x20, 0x26), name: NAME, version: 9 }).copy(info, FIRST_NODE);
    return info;
}

function addStation(info, node, index = 1) {
    const next = Buffer.from(info);
    next[NODE_COUNT]++;
    node.copy(next, FIRST_NODE + index * NODE_SIZE);
    next[FIRST_NODE + index * NODE_SIZE + 10] = index;
    return next;
}

async function native(port, name) {
    const tcp = net.connect(port, '127.0.0.1');
    const udp = dgram.createSocket('udp4');
    const frames = [], datagrams = [];
    let pending = Buffer.alloc(0);
    tcp.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 3 && pending.length >= pending.readUInt16LE(0) + 2) {
            const end = pending.readUInt16LE(0) + 2;
            frames.push({ type: pending[2], body: pending.subarray(3, end) });
            pending = pending.subarray(end);
        }
    });
    udp.on('message', message => datagrams.push(message));
    udp.bind(0, '127.0.0.1');
    await Promise.all([once(tcp, 'connect'), once(udp, 'listening')]);
    const client = {
        datagrams,
        send(type, body = Buffer.alloc(0)) {
            const header = Buffer.alloc(3);
            header.writeUInt16LE(body.length + 1);
            header[2] = type;
            tcp.write(Buffer.concat([header, body]));
        },
        wait(type, predicate = () => true) {
            return until(() => take(frames, entry => entry.type === type && predicate(entry.body))?.body,
                `${name}: relay frame ${type.toString(16)} missing`);
        },
        async membership() {
            const body = await client.wait(T.ROOM);
            client.code = body.subarray(0, 6);
            client.index = body[6];
            client.ip = body.readUInt32LE(7);
            client.token = body.readUInt32LE(11);
            const reg = Buffer.alloc(13);
            reg.writeUInt32LE(MAGIC);
            reg[4] = 4;
            reg.writeUInt32LE(client.token, 5);
            reg.writeUInt32LE(12, 9);
            udp.send(reg, port, '127.0.0.1');
            await until(() => take(datagrams, m => m[4] === 0x84), 'Switch UDP registration missing');
        },
        sendData(dst, body) {
            const header = Buffer.alloc(17);
            header.writeUInt32LE(MAGIC);
            header[4] = 2;
            header.writeUInt32LE(client.token, 5);
            header.writeUInt32LE(dst, 9);
            header.writeUInt16LE(14001, 13);
            header.writeUInt16LE(14002, 15);
            udp.send(Buffer.concat([header, body]), port, '127.0.0.1');
        },
        waitData(body) {
            return until(() => take(datagrams, m => m[4] === 0x82 && m.subarray(13).equals(body)),
                `${name}: expected Switch game datagram`);
        },
        close() { tcp.destroy(); udp.close(); },
    };
    return client;
}

async function ryuldn(port) {
    const socket = net.connect(port, '127.0.0.1');
    const frames = [];
    socket.on('data', Ryu.streamDecoder((type, body) => frames.push({ type, body })));
    await once(socket, 'connect');
    const client = {
        send(type, body) { socket.write(Ryu.frame(type, body)); },
        pending(type) { return frames.some(frame => frame.type === type); },
        wait(type) {
            return until(() => take(frames, f => f.type === type)?.body, `RyuLDN frame ${type} missing`);
        },
        async scan() {
            client.send(Ryu.TYPE.SCAN, Buffer.alloc(Ryu.SIZE.SCAN));
            await client.wait(Ryu.TYPE.SCAN_END);
            return frames.splice(0).filter(f => f.type === Ryu.TYPE.SCAN_REPLY).map(f => f.body);
        },
        sendData(dst, payload) {
            const header = Ryu.encodeProxyInfo({ src: client.ip, dst, sport: 14002, dport: 14001, protocol: 17 },
                Ryu.SIZE.PROXY_DATA);
            header.writeUInt32LE(payload.length, Ryu.SIZE.PROXY_INFO);
            client.send(Ryu.TYPE.PROXY_DATA, Buffer.concat([header, payload]));
        },
        close() { socket.destroy(); },
    };
    client.send(Ryu.TYPE.INITIALIZE, Buffer.alloc(22));
    assert.equal((await client.wait(Ryu.TYPE.INITIALIZE)).length, 22);
    return client;
}

async function nativeHost(client, game, marker) {
    client.send(T.CREATE, Buffer.concat([game, nickname(`Switch ${marker}`), Buffer.from([3])]));
    await client.membership();
    client.info = networkInfo(client.ip, marker);
    client.send(T.PUBLISH, client.info);
    client.send(T.FIND, game);
    await client.wait(T.GROUPS, body => body[0] > 0);
}

async function lanClient(port, ip) {
    const socket = dgram.createSocket('udp4');
    const answers = [], syncs = [], datagrams = [], destinations = [], fragments = [];
    let connection;
    function send(packet) {
        socket.send(Buffer.concat([Buffer.from([Lan.P.IPV4]), packet]), port, '127.0.0.1');
    }
    socket.on('message', message => {
        if (message[0] === Lan.P.IPV4_FRAG) { fragments.push(message.subarray(1)); return; }
        if (message[0] !== Lan.P.IPV4) return;
        const packet = Lan.parseIpv4(message.subarray(1));
        if (!packet) return;
        if (packet.mf || packet.off) { fragments.push(packet.body); return; }
        if (packet.proto === 6) { connection?.input(packet.body); return; }
        if (packet.proto !== 17 || packet.body.length < 8) return;
        if (packet.body.readUInt16BE(2) !== 11452) {
            datagrams.push(packet.body.subarray(8));
            destinations.push({ payload: packet.body.subarray(8), dst: packet.dst });
            return;
        }
        for (const reply of Lan.ldnRead({}, packet.body.subarray(8))) {
            if (reply.type === Lan.LT.SCAN_RESP) answers.push(reply.data);
        }
    });
    socket.bind(0, '127.0.0.1');
    await once(socket, 'listening');
    return {
        scan() {
            send(Lan.udpPacket(ip, 11452, Lan.BCAST, 11452, Lan.ldnFrame(Lan.LT.SCAN)));
        },
        join(host) {
            const mac = Buffer.from('020000000000', 'hex');
            mac.writeUInt32BE(ip, 2);
            const node = Ryu.nodeInfo({ ip, mac, name: NAME, version: 9 });
            connection = new Lan.Tcp(ip, 37451, host, 11452, send);
            const pending = {};
            connection.onOpen = () => connection.write(Lan.ldnFrame(Lan.LT.CONNECT, node));
            connection.onData = data => {
                for (const message of Lan.ldnRead(pending, data)) {
                    if (message.type === Lan.LT.SYNC) syncs.push(message.data);
                }
            };
            connection.connect();
        },
        sendData(dst, data) { send(Lan.udpPacket(ip, 14001, dst, 14002, data)); },
        sendFragments(dst) {
            const payload = Lan.udpPacket(ip, 14001, dst, 14002, Buffer.alloc(1600, 42));
            for (const f of Lan.fragmentIpv4(payload)) send(f);
            const body = Lan.udpPacket(ip, 14001, dst, 14002, Buffer.from('LAN Play relay fragment'));
            const header = Buffer.alloc(16);
            header.writeUInt32BE(ip);
            header.writeUInt32BE(dst, 4);
            header.writeUInt16LE(1, 8);
            header[11] = 1;
            header.writeUInt16LE(body.length, 12);
            header.writeUInt16LE(body.length, 14);
            socket.send(Buffer.concat([Buffer.from([Lan.P.IPV4_FRAG]), header, body]), port, '127.0.0.1');
        },
        answers, syncs, datagrams, destinations, fragments, ip,
        close() { connection?.finish(); socket.close(); },
    };
}

test('Switch, RyuLDN and LAN Play share an isolated game with UDP and a native to RyuLDN TCP stream', { timeout: 20000 }, async t => {
    const stateDir = await fs.mkdtemp(path.join(os.tmpdir(), 'roomcircuit-emulator-'));
    const ryuPort = await freeTcpPort();
    const lanPort = await freeTcpPort();
    const relay = spawn(process.execPath, [path.join(__dirname, 'server.js'), '--port', '0'], {
        stdio: ['ignore', 'pipe', 'pipe'],
        env: { ...process.env, LANYWHERE_STATE_DIR: stateDir, LANYWHERE_STATS_PORT: '0', LANYWHERE_PUBLIC_PORT: '0',
            LANYWHERE_SHUTDOWN_GRACE_MS: '0', ROOMCIRCUIT_EDEN_PORT: '0', ROOMCIRCUIT_RYULDN_PORT: String(ryuPort),
            LANYWHERE_LANPLAY_PORT: String(lanPort) },
    });
    const clients = [];
    t.after(async () => {
        for (const client of clients) client.close();
        if (relay.exitCode === null) {
            const finished = once(relay, 'exit');
            relay.kill('SIGTERM');
            await finished;
        }
        await fs.rm(stateDir, { recursive: true, force: true });
    });
    let logs = '';
    let errors = '';
    relay.stdout.on('data', chunk => { logs += chunk; });
    relay.stderr.on('data', chunk => { errors += chunk; });
    const port = await until(() => {
        if (relay.exitCode !== null) throw new Error(errors || logs);
        const found = logs.match(/datos UDP en (\d+)/);
        if (found) return Number(found[1]);
    }, 'relay start failed');
    const host = await native(port, 'host'); clients.push(host);
    const other = await native(port, 'other floor'); clients.push(other);
    const emu = await ryuldn(ryuPort); clients.push(emu);
    const game = Buffer.alloc(8);
    game.writeBigUInt64LE(GAME);
    await nativeHost(host, game, 1);
    await nativeHost(other, game, 2);
    const rooms = await emu.scan();
    assert.equal(rooms.length, 2);
    const picked = rooms.find(room => room[0x10] === 1);
    assert.equal(picked.readUInt32LE(FIRST_NODE), host.ip);
    const connection = Buffer.alloc(Ryu.SIZE.CONNECT);
    NAME.copy(connection, 0x44);
    connection.writeUInt32LE(9, 0x74);
    picked.copy(connection, 0x7c);
    emu.send(Ryu.TYPE.CONNECT, connection);
    const conn = await host.wait(T.CONTROL, frame => frame[1] === LDN.CONNECT);
    const hostInfo = addStation(picked, conn.subarray(2));
    // The relay seat exists before the host game admits its guest. An early
    // SYNC cannot stand in for PUBLISH, and a delayed host-only SYNC can
    // still arrive after publication. Only the final pair grants admission.
    host.send(T.RELAY, Buffer.concat([Buffer.from([conn[0], LDN.SYNC]), hostInfo]));
    host.send(0x05);
    await host.wait(0x86);
    emu.send(Ryu.TYPE.PING, Buffer.from([1, 1]));
    await emu.wait(Ryu.TYPE.PING);
    assert.equal(emu.pending(Ryu.TYPE.CONNECTED), false, 'premature SYNC must wait for host admission');
    host.send(T.PUBLISH, hostInfo);
    host.send(T.RELAY, Buffer.concat([Buffer.from([conn[0], LDN.SYNC]), picked]));
    host.send(0x05);
    await host.wait(0x86);
    emu.send(Ryu.TYPE.PING, Buffer.from([1, 2]));
    await emu.wait(Ryu.TYPE.PING);
    assert.equal(emu.pending(Ryu.TYPE.CONNECTED), false, 'delayed host-only SYNC cannot complete a guest join');
    host.send(T.RELAY, Buffer.concat([Buffer.from([conn[0], LDN.SYNC]), hostInfo]));
    emu.ip = (await emu.wait(Ryu.TYPE.PROXY_CONFIG)).readUInt32LE();
    assert.equal(emu.ip, conn.readUInt32LE(2));
    assert.equal((await emu.wait(Ryu.TYPE.CONNECTED))[NODE_COUNT], 2);
    assert.notEqual(emu.ip, other.ip);

    const fromEmu = Buffer.from('emulator multicast');
    emu.sendData(BCAST, fromEmu);
    assert.equal((await host.waitData(fromEmu)).readUInt32LE(5), emu.ip);
    const fromSwitch = Buffer.from('Switch unicast');
    host.sendData(emu.ip, fromSwitch);
    const fromSwitchProxy = await emu.wait(Ryu.TYPE.PROXY_DATA);
    assert.equal(Ryu.proxyInfo(fromSwitchProxy).src, host.ip);
    assert.deepEqual(fromSwitchProxy.subarray(Ryu.SIZE.PROXY_DATA), fromSwitch);
    const proxyRoute = { src: emu.ip, dst: host.ip, sport: 15002, dport: 15001, protocol: 6 };
    const streamId = { id: 1, nonce: 42n, owner: 0 };
    host.send(Stream.COMMAND, Stream.encode(emu.ip, { ...streamId, op: Stream.OP.OPEN,
        aux: Stream.MAX_WINDOW, srcPort: 15001, dstPort: 15002 }, hostInfo.subarray(0, 32)));
    const connect = await emu.wait(Ryu.TYPE.PROXY_CONNECT);
    assert.deepEqual(Ryu.proxyInfo(connect), {
        src: host.ip, dst: emu.ip, sport: 15001, dport: 15002, protocol: 6,
    });
    emu.send(Ryu.TYPE.PROXY_CONNECT_REPLY, Ryu.encodeProxyInfo(proxyRoute));
    await host.wait(Stream.RESPONSE, body => Stream.parse(body)?.op === Stream.OP.OPEN_OK);
    const switchStream = Buffer.from('Switch stream bytes');
    host.send(Stream.COMMAND, Stream.encode(emu.ip, { ...streamId, op: Stream.OP.DATA }, switchStream));
    assert.deepEqual((await emu.wait(Ryu.TYPE.PROXY_DATA)).subarray(Ryu.SIZE.PROXY_DATA), switchStream);
    const emulatorStream = Buffer.from('RyuLDN stream bytes');
    const proxyData = Ryu.encodeProxyInfo(proxyRoute, Ryu.SIZE.PROXY_DATA);
    proxyData.writeUInt32LE(emulatorStream.length, Ryu.SIZE.PROXY_INFO);
    emu.send(Ryu.TYPE.PROXY_DATA, Buffer.concat([proxyData, emulatorStream]));
    const inbound = await host.wait(Stream.RESPONSE, body => Stream.parse(body)?.op === Stream.OP.DATA);
    assert.deepEqual(Stream.parse(inbound).payload, emulatorStream);
    emu.sendData(other.ip, Buffer.from('must not reach other room'));
    host.send(T.LEAVE);
    await emu.wait(Ryu.TYPE.DISCONNECT);
    assert.equal(other.datagrams.length, 0);

    const create = Buffer.alloc(Ryu.SIZE.CREATE);
    NAME.copy(create, 0x44);
    create.writeBigUInt64LE(GAME, 0x74);
    create[0x86] = 4;
    create.writeUInt16LE(9, 0x88);
    emu.send(Ryu.TYPE.CREATE, create);
    assert.equal((await emu.wait(Ryu.TYPE.PROXY_CONFIG)).readUInt32LE(), emu.ip);
    const emuInfo = await emu.wait(Ryu.TYPE.CONNECTED);
    const lan = await lanClient(lanPort, 0x0a0d0064);
    clients.push(lan);
    lan.scan();
    const lanVisible = await until(() => lan.answers.find(info => info.readUInt32LE(FIRST_NODE) === emu.ip),
        'LAN Play client cannot see the RyuLDN host');
    assert.deepEqual(lanVisible, emuInfo);
    host.send(T.FIND, game);
    const nativeList = await host.wait(T.GROUPS);
    assert.equal(nativeList[0], 2);
    let emuRoomCode;
    for (let pos = 1; pos < nativeList.length; pos += 6 + Ryu.SIZE.NETWORK) {
        if (nativeList.readUInt32LE(pos + 6 + FIRST_NODE) === emu.ip)
            emuRoomCode = nativeList.subarray(pos, pos + 6);
    }
    assert.ok(emuRoomCode, 'RyuLDN host is missing from the Switch in-game scan');
    host.send(T.JOIN, Buffer.concat([emuRoomCode, nickname('Native visitor'), game, Buffer.from([3])]));
    await host.membership();
    const nativeNode = Ryu.nodeInfo({ ip: host.ip, mac: Buffer.from('020001020304', 'hex'), name: NAME, version: 9 });
    host.send(T.RELAY, Buffer.concat([Buffer.from([0, LDN.CONNECT]), nativeNode]));
    const hostSync = await emu.wait(Ryu.TYPE.SYNC);
    assert.equal(hostSync[NODE_COUNT], 2);
    assert.equal((await host.wait(T.CONTROL, data => data[1] === LDN.SYNC))[2 + NODE_COUNT], 2);
    emu.sendData(host.ip, fromEmu);
    assert.equal((await host.waitData(fromEmu)).readUInt32LE(5), emu.ip);
    host.sendData(emu.ip, fromSwitch);
    assert.deepEqual((await emu.wait(Ryu.TYPE.PROXY_DATA)).subarray(Ryu.SIZE.PROXY_DATA), fromSwitch);

    lan.join(emu.ip);
    assert.equal((await emu.wait(Ryu.TYPE.SYNC))[NODE_COUNT], 3);
    await until(() => lan.syncs.some(info => info[NODE_COUNT] === 3), 'LAN Play LDN join missing');
    const fromLan = Buffer.from('LAN Play unicast');
    lan.sendData(emu.ip, fromLan);
    assert.deepEqual((await emu.wait(Ryu.TYPE.PROXY_DATA)).subarray(Ryu.SIZE.PROXY_DATA), fromLan);
    emu.sendData(lan.ip, fromEmu);
    await until(() => take(lan.datagrams, data => data.equals(fromEmu)), 'RyuLDN to LAN Play UDP missing');

    const legacy = await lanClient(lanPort, 0x0a0d0065);
    const legacy2 = await lanClient(lanPort, 0x0a0d0066);
    clients.push(legacy, legacy2);
    legacy.scan();
    legacy2.scan();
    await until(() => legacy.answers.length > 0 && legacy2.answers.length > 0, 'legacy addresses not learned');
    const legacyData = Buffer.from('unattached LAN Play peers');
    legacy.sendData(legacy2.ip, legacyData);
    await until(() => take(legacy2.datagrams, data => data.equals(legacyData)), 'legacy LAN Play peers regressed');
    lan.sendData(legacy.ip, Buffer.from('must not cross from attached room'));
    legacy.sendData(lan.ip, Buffer.from('must not cross into attached room'));
    legacy.sendFragments(lan.ip);
    lan.sendFragments(legacy.ip);
    lan.sendData(BCAST, fromLan);
    assert.deepEqual((await emu.wait(Ryu.TYPE.PROXY_DATA)).subarray(Ryu.SIZE.PROXY_DATA), fromLan);
    await sleep(100);
    assert.equal(legacy.datagrams.length, 0);
    assert.equal(lan.datagrams.length, 0);
    assert.equal(legacy.fragments.length, 0);
    assert.equal(lan.fragments.length, 0);

    // A routed phone substitutes the relay gateway for a local game broadcast.
    // The reserved address is shared by all games; only admitted seats may use it.
    const mobile = Buffer.from('iOS routed broadcast');
    legacy.sendData(Lan.GATEWAY_IP, Buffer.from('unadmitted'));
    legacy.sendFragments(Lan.GATEWAY_IP);
    legacy2.join(emu.ip);
    assert.equal((await emu.wait(Ryu.TYPE.SYNC))[NODE_COUNT], 4);
    await until(() => legacy2.syncs.some(info => info[NODE_COUNT] === 4), 'second LAN seat was not admitted');
    lan.sendData(Lan.GATEWAY_IP, mobile);
    const routed = await emu.wait(Ryu.TYPE.PROXY_DATA);
    assert.equal(Ryu.proxyInfo(routed).dst, BCAST);
    assert.deepEqual(routed.subarray(Ryu.SIZE.PROXY_DATA), mobile);
    await host.waitData(mobile);
    await until(() => take(legacy2.datagrams, data => data.equals(mobile)), 'routed broadcast did not reach LAN peer');
    assert.equal(legacy2.destinations.find(row => row.payload.equals(mobile)).dst, legacy2.ip);

    lan.sendFragments(Lan.GATEWAY_IP);
    const fragments = await emu.wait(Ryu.TYPE.PROXY_DATA);
    assert.deepEqual(fragments.subarray(Ryu.SIZE.PROXY_DATA), Buffer.alloc(1600, 42));
    const relayFragments = await emu.wait(Ryu.TYPE.PROXY_DATA);
    assert.deepEqual(relayFragments.subarray(Ryu.SIZE.PROXY_DATA), Buffer.from('LAN Play relay fragment'));
    await until(() => take(legacy2.datagrams, data => data.equals(Buffer.from('LAN Play relay fragment'))),
        'encapsulated LAN fragments failed to reach an admitted LAN peer');
    assert.equal(legacy.datagrams.length, 0, 'routed broadcast leaked to unrelated LAN endpoint');
    assert.equal(legacy.fragments.length, 0);
    assert.equal(lan.datagrams.length, 0, 'routed broadcast echoed to sender');
    assert.equal(other.datagrams.length, 0, 'routed broadcast leaked to another game');

    host.send(T.LEAVE);
    assert.equal((await emu.wait(Ryu.TYPE.SYNC))[NODE_COUNT], 3);
});
