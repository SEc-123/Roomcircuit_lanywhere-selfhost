// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomBytes } = require('node:crypto');
const { once } = require('node:events');
const fs = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const { zstdDecompressSync } = require('node:zlib');

const Ryu = require('../ryuldn/protocol');
const { client, uint, waitFor, anyIp } = require('./test-peer');
const {
    GAME, VERSION, FIRST_NODE, POLL_SOURCE, game, ldn, recvLdn, nodeInfo, networkInfo,
    withStations, take, reserveUdpPort,
} = require('./test-crossplay');

const { TYPE, SIZE } = Ryu;
const BCAST = 0x0a0dffff;
const BIN = path.resolve(process.argv[2] ?? path.join(__dirname, 'build'));

async function freeTcpPort() {
    const server = net.createServer().listen(0, '127.0.0.1');
    await once(server, 'listening');
    const { port } = server.address();
    await new Promise(resolve => server.close(resolve));
    return port;
}

async function ryuPeer(port, name, clients) {
    const socket = net.connect(port, '127.0.0.1');
    clients.push({ close: () => socket.destroy() });
    const frames = [];
    socket.on('data', Ryu.streamDecoder((type, body) => frames.push({ type, body })));
    await once(socket, 'connect');
    const peer = {
        frames,
        send(type, body) { socket.write(Ryu.frame(type, body)); },
        wait(type, predicate = () => true) {
            return waitFor(() => take(frames, f => f.type === type && predicate(f.body))?.body,
                name + ': RyuLDN frame ' + type);
        },
        async membership() {
            const config = await this.wait(TYPE.PROXY_CONFIG);
            this.ip = uint(config.readUInt32LE(), 4);
            return this.wait(TYPE.CONNECTED);
        },
        async host() {
            const body = Buffer.alloc(SIZE.CREATE);
            GAME.copy(body, 0x74);
            body.write(name, 0x44);
            body[0x86] = 4;
            body.writeUInt16LE(VERSION, 0x88);
            this.send(TYPE.CREATE, body);
            return this.membership();
        },
        async rooms() {
            const query = Buffer.alloc(SIZE.SCAN);
            GAME.copy(query);
            query[0x5c] = 1;
            this.send(TYPE.SCAN, query);
            await this.wait(TYPE.SCAN_END);
            const rooms = [];
            let item;
            while ((item = take(frames, f => f.type === TYPE.SCAN_REPLY))) rooms.push(item.body);
            return rooms;
        },
        join(info) {
            const body = Buffer.alloc(SIZE.CONNECT);
            body.write(name, 0x44);
            body.writeUInt32LE(VERSION, 0x74);
            info.copy(body, 0x7c);
            this.send(TYPE.CONNECT, body);
        },
        data(to, payload) {
            const header = Ryu.encodeProxyInfo({
                src: this.ip.readUInt32BE(), dst: to.readUInt32BE(), sport: 32123, dport: 54321, protocol: 17,
            }, SIZE.PROXY_DATA);
            header.writeUInt32LE(payload.length, SIZE.PROXY_INFO);
            this.send(TYPE.PROXY_DATA, Buffer.concat([header, payload]));
        },
    };
    peer.send(TYPE.INITIALIZE, Buffer.alloc(22));
    await peer.wait(TYPE.INITIALIZE);
    return peer;
}

async function edenPeer(port, name, clients) {
    const peer = client(port, BIN);
    clients.push(peer);
    await waitFor(() => peer.connected, 'ENet connect');
    peer.join(name);
    peer.ip = (await waitFor(() => take(peer.messages, m => m[0] === 2), 'Eden membership')).subarray(1);
    return peer;
}

async function yuzuPeer(port, name, clients) {
    const peer = await edenPeer(port, name, clients);
    peer.udpProtocol = 3;
    return peer;
}

function scan(peer) { ldn(peer, 0, anyIp, Buffer.alloc(0), 1); }

async function joinRyuHost(eden, ryu, expectedCount = 2) {
    scan(eden);
    const result = (await recvLdn(eden, 1, ryu.ip)).subarray(15);
    assert.equal(result.readUInt32LE(FIRST_NODE), ryu.ip.readUInt32BE());
    ldn(eden, 2, ryu.ip, nodeInfo(eden.ip, 'Eden guest'));
    const updated = await ryu.wait(TYPE.SYNC, body => body[0x67] === expectedCount);
    assert.deepEqual((await recvLdn(eden, 3, ryu.ip)).subarray(15), updated);
}

async function exchange(eden, ryu, length) {
    const toRyu = randomBytes(length);
    game(eden, ryu.ip, toRyu);
    const received = await ryu.wait(TYPE.PROXY_DATA);
    assert.deepEqual(Ryu.proxyInfo(received), {
        src: eden.ip.readUInt32BE(), dst: ryu.ip.readUInt32BE(), sport: 3756, dport: 7788, protocol: 17,
    });
    assert.deepEqual(received.subarray(SIZE.PROXY_DATA), toRyu);
    const toEden = randomBytes(length);
    ryu.data(eden.ip, toEden);
    const packet = await waitFor(() => take(eden.messages, m => m[0] === 5), 'Eden receive compressed game data');
    assert.deepEqual(packet.subarray(2, 6), ryu.ip);
    assert.deepEqual(packet.subarray(9, 13), eden.ip);
    assert.equal(packet.readUInt16BE(6), 32123);
    assert.equal(packet.readUInt16BE(13), 54321);
    assert.equal(packet[15], eden.udpProtocol ?? 4);
    assert.deepEqual(zstdDecompressSync(packet.subarray(21)), toEden);
}

async function sendBetweenEnetClients(from, to) {
    const bytes = randomBytes(8192);
    game(from, to.ip, bytes);
    const got = await waitFor(() => take(to.messages, message => message[0] === 5), 'ENet game datagram');
    assert.deepEqual(got.subarray(2, 6), from.ip);
    assert.deepEqual(got.subarray(9, 13), to.ip);
    assert.equal(got[15], to.udpProtocol ?? 4, 'Game data must use the recipient UDP enum');
    assert.deepEqual(zstdDecompressSync(got.subarray(21)), bytes);
}

async function rotatedEnetHosts(edenPort, yuzuPort, ryuPort, clients) {
    const eden = await edenPeer(edenPort, 'EdenRotate', clients);
    const yuzu = await yuzuPeer(yuzuPort, 'YuzuRotate', clients);
    const observer = await ryuPeer(ryuPort, 'Room observer', clients);
    for (const [host, guest] of [[eden, yuzu], [yuzu, eden]]) {
        const info = networkInfo(nodeInfo(host.ip, 'ENet host'), host.udpProtocol === 3 ? 93 : 92);
        // Discard polls received before the game switched to the hosting role.
        host.messages.splice(0);
        await recvLdn(host, 0, POLL_SOURCE);
        ldn(host, 1, POLL_SOURCE, info);
        // A scan reply from an unrelated room can already be queued after the previous
        // role. Use an independent client to observe this exact publication before
        // giving the player one discovery attempt.
        const deadline = Date.now() + 3000;
        for (;;) {
            const listings = await observer.rooms();
            if (listings.some(row => row.readUInt32LE(FIRST_NODE) === host.ip.readUInt32BE() &&
                row.subarray(0, 38).equals(info.subarray(0, 38)))) break;
            assert(Date.now() < deadline, 'New host failed to publish its selected session to the shared relay');
            await delay(300);
        }
        guest.messages.splice(0);
        scan(guest);
        const visible = (await recvLdn(guest, 1, host.ip)).subarray(15);
        assert.deepEqual(visible.subarray(0, 38), info.subarray(0, 38));
        ldn(guest, 2, host.ip, nodeInfo(guest.ip, 'ENet guest'));
        const join = await recvLdn(host, 2, guest.ip);
        ldn(host, 3, guest.ip, withStations(info, join.subarray(15)));
        const synced = await recvLdn(guest, 3, host.ip);
        assert.equal(synced[15 + 0x67], 2);
        await sendBetweenEnetClients(guest, host);
        await sendBetweenEnetClients(host, guest);
        ldn(host, 5, guest.ip);
        await recvLdn(guest, 5, host.ip);
    }
    console.log('Original Yuzu and Eden exchange game UDP when either owns the room');
}

async function main(legacyYuzu) {
    const [edenPort, ryuPort] = await Promise.all([reserveUdpPort(), freeTcpPort()]);
    let yuzuPort = await reserveUdpPort();
    while (yuzuPort === edenPort) yuzuPort = await reserveUdpPort();
    const roomPeer = legacyYuzu ? yuzuPeer : edenPeer;
    const roomPort = legacyYuzu ? yuzuPort : edenPort;
    const name = legacyYuzu ? 'Original Yuzu' : 'Eden';
    const stateDir = await fs.mkdtemp(path.join(os.tmpdir(), 'rc-eden-ryuldn-'));
    const relay = spawn(process.execPath, [path.join(__dirname, '../relay/server.js'), '--port', '0'], {
        stdio: ['ignore', 'pipe', 'inherit'],
        env: { ...process.env, LANYWHERE_STATE_DIR: stateDir, LANYWHERE_OPS_PORT: '0',
            LANYWHERE_STATS_PORT: '0', LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_LANPLAY_PORT: '0',
            LANYWHERE_CONTROL_BIND_ADDRESS: '127.0.0.1', LANYWHERE_UDP_BIND_ADDRESS: '127.0.0.1',
            ROOMCIRCUIT_EMULATOR_BIND: '127.0.0.1', ROOMCIRCUIT_EDEN_PORT: String(edenPort),
            ROOMCIRCUIT_YUZU_PORT: String(yuzuPort),
            ROOMCIRCUIT_RYULDN_PORT: String(ryuPort), ROOMCIRCUIT_ENET_BINARY: path.join(BIN, 'enet-host'),
            LANYWHERE_SHUTDOWN_GRACE_MS: '0' },
    });
    const exited = once(relay, 'close');
    const clients = [];
    let logs = '', interval;
    relay.stdout.on('data', bytes => { logs += bytes; });
    try {
        await waitFor(() => logs.includes('datos UDP en'), 'Crossplay relay ready');
        const eHost = await roomPeer(roomPort, 'ENetHost', clients);
        const eGuest = await roomPeer(roomPort, 'ENetGuest', clients);
        const rHost = await ryuPeer(ryuPort, 'RyuHost', clients);
        const rGuest = await ryuPeer(ryuPort, 'RyuGuest', clients);
        await rHost.host();
        let current = networkInfo(nodeInfo(eHost.ip, 'Eden Host'), 81);
        await recvLdn(eHost, 0, POLL_SOURCE);
        ldn(eHost, 1, POLL_SOURCE, current);
        interval = setInterval(() => {
            const poll = take(eHost.messages, m => m[0] === 6 && m[1] === 0);
            if (poll && current) ldn(eHost, 1, POLL_SOURCE, current);
        }, 60);

        await joinRyuHost(eGuest, rHost);
        const listings = await rGuest.rooms();
        assert.equal(listings.length, 2);
        const chosen = listings.find(room => room.readUInt32LE(FIRST_NODE) === eHost.ip.readUInt32BE());
        assert.deepEqual(chosen.subarray(0, 38), current.subarray(0, 38));
        rGuest.join(chosen);
        const incoming = await recvLdn(eHost, 2);
        const joiner = incoming.subarray(2, 6);
        assert.equal(incoming.readUInt32LE(15), joiner.readUInt32BE());
        current = withStations(current, incoming.subarray(15));
        ldn(eHost, 3, joiner, current);
        const synced = await rGuest.membership();
        assert.deepEqual(rGuest.ip, joiner);
        assert.equal(synced[0x67], 2);
        for (const length of [128, 8192, 65507]) {
            await exchange(eGuest, rHost, length);
            await exchange(eHost, rGuest, length);
        }
        console.log(name + ' and RyuLDN can each host and join the other, with bidirectional UDP up to 65,507 bytes');

        // Send across rooms first, then use successful traffic as a barrier on each transport.
        eGuest.messages.length = 0; rGuest.frames.length = 0;
        game(eGuest, rGuest.ip, Buffer.from('cross-room'));
        rGuest.data(eGuest.ip, Buffer.from('cross-room'));
        game(eGuest, anyIp, Buffer.from('public discovery'), true);
        const broadcast = await rHost.wait(TYPE.PROXY_DATA);
        assert.equal(Ryu.proxyInfo(broadcast).dst, BCAST);
        rGuest.data(anyIp, Buffer.from('Ryu discovery'));
        const edenBroadcast = await waitFor(() => take(eHost.messages, m => m[0] === 5), 'Eden receives room broadcast');
        assert.equal(edenBroadcast[16], 1);
        assert.deepEqual(zstdDecompressSync(edenBroadcast.subarray(21)), Buffer.from('Ryu discovery'));
        await exchange(eGuest, rHost, 100);
        await exchange(eHost, rGuest, 100);
        assert(!eGuest.messages.some(m => m[0] === 5) && !rGuest.frames.some(m => m.type === TYPE.PROXY_DATA));

        rHost.send(TYPE.DISCONNECT, Buffer.alloc(4));
        await recvLdn(eGuest, 5, rHost.ip);
        rGuest.send(TYPE.DISCONNECT, Buffer.alloc(4));
        const departed = await recvLdn(eHost, 4, rGuest.ip);
        assert.deepEqual(departed.subarray(15 + 4, 15 + 10), incoming.subarray(15 + 4, 15 + 10));
        current = networkInfo(nodeInfo(eHost.ip, 'Eden Host'), 81);
        ldn(eHost, 1, POLL_SOURCE, current);
        await rHost.host();
        // No new scan since the old Ryu room closed: same host IP, new session ID.
        ldn(eGuest, 2, rHost.ip, nodeInfo(eGuest.ip, 'stale'));
        await recvLdn(eGuest, 5, rHost.ip);
        await joinRyuHost(eGuest, rHost);
        await exchange(eGuest, rHost, 4096);
        console.log('Identical titles keep broadcasts and unicast isolated, and stale rooms refuse reconnection');

        const rehosted = (await rGuest.rooms()).find(room => room.readUInt32LE(FIRST_NODE) === rHost.ip.readUInt32BE());
        rGuest.join(rehosted);
        assert.equal((await rGuest.membership())[0x67], 3);
        await rHost.wait(TYPE.SYNC, body => body[0x67] === 3);
        const sameRyu = randomBytes(4096);
        rGuest.data(rHost.ip, sameRyu);
        assert.deepEqual((await rHost.wait(TYPE.PROXY_DATA)).subarray(SIZE.PROXY_DATA), sameRyu);
        rHost.data(rGuest.ip, sameRyu);
        assert.deepEqual((await rGuest.wait(TYPE.PROXY_DATA)).subarray(SIZE.PROXY_DATA), sameRyu);

        const sameEden = await roomPeer(roomPort, 'ENetSecond', clients);
        scan(sameEden);
        await recvLdn(sameEden, 1, eHost.ip);
        ldn(sameEden, 2, eHost.ip, nodeInfo(sameEden.ip, 'Eden second'));
        const eNode = await recvLdn(eHost, 2, sameEden.ip);
        current = withStations(current, eNode.subarray(15));
        ldn(eHost, 3, sameEden.ip, current);
        await recvLdn(sameEden, 3, eHost.ip);
        await sendBetweenEnetClients(sameEden, eHost);
        await sendBetweenEnetClients(eHost, sameEden);
        console.log(name + ' same-profile and RyuLDN same-profile rooms transport UDP in both directions');
        if (legacyYuzu) await rotatedEnetHosts(edenPort, yuzuPort, ryuPort, clients);
    } catch (error) {
        console.error(logs);
        throw error;
    } finally {
        clearInterval(interval);
        await Promise.allSettled(clients.map(peer => peer.close()));
        relay.kill('SIGTERM');
        await exited;
        await fs.rm(stateDir, { recursive: true, force: true });
    }
}

if (require.main === module)
    main(false).then(() => main(true)).catch(error => { console.error(error); process.exitCode = 1; });

module.exports = { ryuPeer, edenPeer, yuzuPeer, freeTcpPort };
