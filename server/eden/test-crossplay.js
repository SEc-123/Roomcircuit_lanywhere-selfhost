// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const dgram = require('node:dgram');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { zstdCompressSync, zstdDecompressSync } = require('node:zlib');
const { client, uint, waitFor, anyIp } = require('./test-peer');
const lanPlay = require('../relay/lanplay');

const binaryDir = path.resolve(process.argv[2] ?? path.join(__dirname, 'build'));
const GAME = Buffer.from('00c0ab0000000001', 'hex');
const FIRST_NODE = 0x68;
const NODE_SIZE = 0x40;
const JOINED = 0x81;
const PUBLISH = 0x07;
const RELAY = 0x04;
const RELAY_IN = 0x84;
const NET13 = Buffer.from([1]);
const CONSOLE_MAGIC = 0x594e414c;
const POLL_SOURCE = Buffer.from([10, 13, 254, 254]);
const VERSION = 42;
const DELAY = ms => new Promise(resolve => setTimeout(resolve, ms));

function restoredIp(network, node = 0) {
    return uint(network.readUInt32LE(FIRST_NODE + node * NODE_SIZE), 4);
}

function take(list, condition) {
    const index = list.findIndex(condition);
    return index === -1 ? undefined : list.splice(index, 1)[0];
}

function nodeInfo(ip, name) {
    const node = Buffer.alloc(NODE_SIZE);
    const address = Buffer.isBuffer(ip) ? ip.readUInt32BE() : ip;
    node.writeUInt32LE(address, 0);
    node[4] = 0x02;
    node.writeUInt32BE(address, 6);
    node[11] = 1;
    node.write(name, 12);
    node.writeUInt16LE(VERSION, 46);
    return node;
}

function networkInfo(node, session) {
    const info = Buffer.alloc(0x480);
    GAME.copy(info);
    info[0x10] = session;
    node.subarray(4, 10).copy(info, 0x20);
    info[0x66] = 8;
    info[0x67] = 1;
    node.copy(info, FIRST_NODE);
    return info;
}

function withStations(info, ...nodes) {
    const copy = Buffer.from(info);
    copy[0x67] = nodes.length + 1;
    for (const [index, node] of nodes.entries()) {
        const offset = FIRST_NODE + (index + 1) * NODE_SIZE;
        node.copy(copy, offset);
        copy[offset + 10] = index + 1;
    }
    return copy;
}

async function consolePeer(port, name) {
    const tcp = net.connect({ port, host: '127.0.0.1' });
    const frames = [], data = [];
    let pending = Buffer.alloc(0), token;
    const udp = dgram.createSocket('udp4');
    tcp.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 3 && pending.length >= pending.readUInt16LE(0) + 2) {
            const length = pending.readUInt16LE(0) + 2;
            frames.push({ message: pending[2], body: pending.subarray(3, length) });
            pending = pending.subarray(length);
        }
    });
    udp.on('message', body => data.push(body));
    await once(tcp, 'connect');
    udp.bind(0, '127.0.0.1');
    await once(udp, 'listening');

    const peer = {
        send(message, body = Buffer.alloc(0)) {
            const header = Buffer.alloc(3);
            header.writeUInt16LE(body.length + 1);
            header[2] = message;
            tcp.write(Buffer.concat([header, body]));
        },
        recv(message, pred = () => true) {
            return waitFor(() => take(frames, frame => frame.message === message && pred(frame.body))?.body,
                `${name} RoomCircuit frame ${message}`);
        },
        async inRoom() {
            const info = await this.recv(JOINED);
            this.ip = info.readUInt32LE(7);
            token = info.readUInt32LE(11);
            const body = Buffer.alloc(13);
            body.writeUInt32LE(CONSOLE_MAGIC);
            body[4] = 0x04;
            body.writeUInt32LE(token, 5);
            body.writeUInt32LE(12, 9);
            udp.send(body, port, '127.0.0.1');
            await waitFor(() => take(data, packet => packet.length === 9 && packet[4] === 0x84),
                `${name} UDP registration`);
        },
        async host(session) {
            const nickname = Buffer.alloc(32);
            nickname.write(name);
            this.send(0x01, Buffer.concat([GAME, nickname, NET13]));
            await this.inRoom();
            this.info = networkInfo(nodeInfo(this.ip, name), session);
            this.send(PUBLISH, this.info);
        },
        async search(session) {
            for (let i = 0; i < 20; i++) {
                this.send(0x08, GAME);
                const body = await this.recv(0x87);
                for (let n = 0; n < body[0]; n++) {
                    const offset = 1 + n * (6 + 0x480);
                    if (body[offset + 6 + 0x10] === session) return body.subarray(offset, offset + 6);
                }
                await DELAY(100);
            }
            assert.fail(`Console didn't find session ${session}`);
        },
        async join(code) {
            const nickname = Buffer.alloc(32);
            nickname.write(name);
            this.send(0x02, Buffer.concat([code, nickname, GAME, NET13]));
            await this.inRoom();
        },
        datagram(dst, payload) {
            const body = Buffer.alloc(17);
            body.writeUInt32LE(CONSOLE_MAGIC);
            body[4] = 0x02;
            body.writeUInt32LE(token, 5);
            body.writeUInt32LE(dst.readUInt32BE(), 9);
            body.writeUInt16LE(3344, 13);
            body.writeUInt16LE(7788, 15);
            udp.send(Buffer.concat([body, payload]), port, '127.0.0.1');
        },
        data,
        close() { tcp.destroy(); udp.close(); },
    };
    return peer;
}

function ldn(client, type, to, payload = Buffer.alloc(0), broadcast = 0) {
    client.command(5, Buffer.concat([
        Buffer.from([6, type]), client.ip, to, Buffer.from([broadcast]), uint(payload.length, 4), payload,
    ]));
}

async function recvLdn(client, type, from) {
    return waitFor(() => take(client.messages, bytes => bytes[0] === 6 && bytes[1] === type &&
        (!from || from.equals(bytes.subarray(2, 6)))), `Eden LDN ${type}`);
}

function game(client, to, data) {
    const compressed = zstdCompressSync(data);
    client.command(5, Buffer.concat([
        Buffer.from([5, 1]), client.ip, uint(3756, 2), Buffer.from([1]), to, uint(7788, 2),
        Buffer.from([4, 0]), uint(compressed.length, 4), compressed,
    ]));
}

async function acceptData(rc, emu) {
    const one = Buffer.from('Eden to RoomCircuit');
    const two = Buffer.from('RoomCircuit to Eden');
    const rcIp = uint(rc.ip, 4);
    game(emu, rcIp, one);
    const atSwitch = await waitFor(() => take(rc.data, bytes => bytes[4] === 0x82),
        'Eden game packet at RoomCircuit guest or host');
    assert.equal(atSwitch.readUInt32LE(5), emu.ip.readUInt32BE());
    assert.equal(atSwitch.readUInt16LE(9), 3756);
    assert.deepEqual(atSwitch.subarray(13), one);
    rc.datagram(emu.ip, two);
    const atEden = await waitFor(() => take(emu.messages, bytes => bytes[0] === 5),
        'RoomCircuit game packet at Eden guest or host');
    assert.deepEqual(atEden.subarray(2, 6), rcIp);
    assert.equal(atEden.readUInt16BE(6), 3344);
    assert.deepEqual(zstdDecompressSync(atEden.subarray(21)), two);
}

async function lanPlayGuest(port, hostIp) {
    const socket = dgram.createSocket('udp4');
    const ip = 0x0a0d0505, scans = [], syncs = [], data = [];
    let session;
    function sendIp(bytes) {
        socket.send(Buffer.concat([Buffer.from([lanPlay.P.IPV4]), bytes]), port, '127.0.0.1');
    }
    function sendDatagram(dst, sport, dport, payload) {
        sendIp(lanPlay.udpPacket(ip, sport, dst, dport, payload));
    }
    const scan = () => sendDatagram(lanPlay.BCAST, 11452, 11452,
        lanPlay.ldnFrame(lanPlay.LT.SCAN));
    socket.on('message', bytes => {
        if (bytes[0] !== lanPlay.P.IPV4) return;
        const packet = lanPlay.parseIpv4(bytes.subarray(1));
        if (!packet) return;
        if (packet.proto === 6) {
            session?.input(packet.body);
            return;
        }
        if (packet.proto !== 17 || packet.body.length < 8) return;
        const dport = packet.body.readUInt16BE(2);
        if (dport !== 11452) {
            data.push({ source: packet.src, payload: packet.body.subarray(8) });
            return;
        }
        for (const frame of lanPlay.ldnRead({}, packet.body.subarray(8))) {
            if (frame.type === lanPlay.LT.SCAN_RESP) scans.push(frame.data);
        }
    });
    socket.bind(0, '127.0.0.1');
    try {
        await once(socket, 'listening');
        scan();
        await waitFor(() => scans.find(result => result.readUInt32LE(FIRST_NODE) === hostIp),
            'LAN Play finds the active Switch and Eden room');
    } catch (error) {
        socket.close();
        throw error;
    }
    session = new lanPlay.Tcp(ip, 50000, hostIp, 11452, sendIp);
    const pending = {};
    session.onOpen = () => session.write(lanPlay.ldnFrame(lanPlay.LT.CONNECT, nodeInfo(ip, 'Ryujinx LAN')));
    session.onData = bytes => {
        for (const frame of lanPlay.ldnRead(pending, bytes)) {
            if (frame.type === lanPlay.LT.SYNC) syncs.push(frame.data);
        }
    };
    session.connect();
    return {
        ip, data, syncs, sendDatagram,
        close() { session.finish(); socket.close(); },
    };
}

async function reserveUdpPort() {
    const sock = dgram.createSocket('udp4');
    sock.bind(0, '127.0.0.1');
    await once(sock, 'listening');
    const port = sock.address().port;
    await new Promise(resolve => sock.close(resolve));
    return port;
}

async function main() {
    const edenPort = await reserveUdpPort();
    let lanPlayPort = await reserveUdpPort();
    while (lanPlayPort === edenPort) lanPlayPort = await reserveUdpPort();
    const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'roomcircuit-eden-e2e-'));
    const relay = spawn(process.execPath, [path.join(__dirname, '../relay/server.js'), '--port', '0'], {
        stdio: ['ignore', 'pipe', 'inherit'],
        env: { ...process.env,
            LANYWHERE_STATE_DIR: stateDir, LANYWHERE_STATS_PORT: '0', LANYWHERE_PUBLIC_PORT: '0',
            LANYWHERE_LANPLAY_PORT: String(lanPlayPort), ROOMCIRCUIT_RYULDN_PORT: '0', ROOMCIRCUIT_EDEN_PORT: String(edenPort),
            ROOMCIRCUIT_ENET_BINARY: path.join(binaryDir, 'enet-host'),
            ROOMCIRCUIT_EMULATOR_BIND: '127.0.0.1', LANYWHERE_CONTROL_BIND_ADDRESS: '127.0.0.1',
            LANYWHERE_UDP_BIND_ADDRESS: '127.0.0.1', LANYWHERE_SHUTDOWN_GRACE_MS: '0',
        },
    });
    const relayExited = once(relay, 'close');
    let logs = '';
    relay.stdout.on('data', data => { logs += data; });
    const clients = [];
    async function openEden(name) {
        const peer = client(edenPort, binaryDir);
        clients.push(peer);
        await waitFor(() => peer.connected, 'Eden ENet handshake');
        peer.join(name);
        const joined = await waitFor(() => take(peer.messages, bytes => bytes[0] === 2), 'Eden lobby join');
        peer.ip = joined.subarray(1);
        return peer;
    }
    try {
        const [, port] = await waitFor(() => logs.match(/datos UDP en (\d+)/), 'RoomCircuit relay binds');
        const rcHost = await consolePeer(Number(port), 'SwitchHost');
        clients.push(rcHost);
        await rcHost.host(1);
        const edenGuest = await openEden('EdenGuest');
        const idleEden = await openEden('EdenIdle');
        const rcIp = uint(rcHost.ip, 4);
        ldn(edenGuest, 0, anyIp, Buffer.alloc(0), 1);
        const room = await recvLdn(edenGuest, 1, rcIp);
        assert.deepEqual(restoredIp(room.subarray(15)), rcIp);
        ldn(edenGuest, 2, rcIp, nodeInfo(edenGuest.ip, 'Guest'));
        const guestNode = await rcHost.recv(RELAY_IN, body => body[1] === 3);
        assert.equal(guestNode.subarray(2).readUInt32LE(), edenGuest.ip.readUInt32BE());
        const consoleSync = withStations(rcHost.info, guestNode.subarray(2));
        rcHost.send(PUBLISH, consoleSync);
        rcHost.send(RELAY, Buffer.concat([Buffer.from([guestNode[0], 4]), consoleSync]));
        const edenSync = await recvLdn(edenGuest, 3, rcIp);
        assert.deepEqual(restoredIp(edenSync.subarray(15), 1), edenGuest.ip);
        await acceptData(rcHost, edenGuest);
        assert(!idleEden.messages.some(packet => packet[0] === 5), 'unrelated Eden peer received gameplay');
        console.log('Switch hosts, Eden finds and joins, game datagrams in both directions');

        const third = await lanPlayGuest(lanPlayPort, rcHost.ip);
        clients.push(third);
        const thirdNode = await rcHost.recv(RELAY_IN, body => body[1] === 3);
        const threePlayers = withStations(rcHost.info, guestNode.subarray(2), thirdNode.subarray(2));
        rcHost.send(PUBLISH, threePlayers);
        for (const seat of [guestNode[0], thirdNode[0]])
            rcHost.send(RELAY, Buffer.concat([Buffer.from([seat, 4]), threePlayers]));
        await recvLdn(edenGuest, 3, rcIp);
        await waitFor(() => third.syncs.some(info => info[0x67] === 3), 'LAN Play sees all players');

        const emuToLan = Buffer.from('Eden to LAN Play within a Switch-hosted room');
        game(edenGuest, uint(third.ip, 4), emuToLan);
        const atLan = await waitFor(() => take(third.data, entry => entry.payload.equals(emuToLan)),
            'gameplay reaches LAN Play directly from Eden');
        assert.equal(atLan.source, edenGuest.ip.readUInt32BE());
        const lanToEmu = Buffer.from('LAN Play to Eden within a Switch-hosted room');
        third.sendDatagram(edenGuest.ip.readUInt32BE(), 3756, 7788, lanToEmu);
        const atEden = await waitFor(() => take(edenGuest.messages, bytes => bytes[0] === 5),
            'gameplay reaches Eden directly from LAN Play');
        assert.equal(atEden.readUInt32BE(2), third.ip);
        assert.deepEqual(zstdDecompressSync(atEden.subarray(21)), lanToEmu);
        await acceptData(rcHost, edenGuest);
        console.log('Switch, Eden and LAN Play share one room and exchange actual wire payloads');

        // Stock Eden only publishes its room when polled with Scan.
        const edenHost = await openEden('EdenHost');
        const poll = await recvLdn(edenHost, 0, POLL_SOURCE);
        assert.equal(poll[10], 1);
        const edenInfo = networkInfo(nodeInfo(edenHost.ip, 'Eden host'), 2);
        ldn(edenHost, 1, POLL_SOURCE, edenInfo);
        const rcGuest = await consolePeer(Number(port), 'SwitchGuest');
        clients.push(rcGuest);
        await rcGuest.join(await rcGuest.search(2));
        const rcGuestIp = uint(rcGuest.ip, 4);
        rcGuest.send(RELAY, Buffer.concat([Buffer.from([0, 3]), nodeInfo(rcGuest.ip, 'Guest')]));
        const fromConsole = await recvLdn(edenHost, 2, rcGuestIp);
        assert.equal(fromConsole.subarray(15).readUInt32LE(), rcGuest.ip);
        const edenNetwork = withStations(edenInfo, fromConsole.subarray(15));
        ldn(edenHost, 3, rcGuestIp, edenNetwork);
        assert.deepEqual(restoredIp((await rcGuest.recv(RELAY_IN, body => body[1] === 4)).subarray(2), 1), rcGuestIp);
        await acceptData(rcGuest, edenHost);

        // Even two populated lobbies for the same game must keep their datagrams apart.
        const rejected = Buffer.from('Cross-room packet must not arrive');
        game(edenGuest, rcGuestIp, rejected);
        rcGuest.datagram(edenGuest.ip, rejected);
        await acceptData(rcHost, edenGuest);
        await acceptData(rcGuest, edenHost);
        assert(!rcGuest.data.some(packet => packet[4] === 0x82 && packet.subarray(13).equals(rejected)));
        assert(!edenGuest.messages.some(packet => packet[0] === 5 && zstdDecompressSync(packet.subarray(21)).equals(rejected)));
        console.log('Independent rooms for the same game stay isolated');

        rcHost.send(0x03);
        await recvLdn(edenGuest, 5, rcIp);
        rcGuest.send(0x03);
        const disconnect = await recvLdn(edenHost, 4, rcGuestIp);
        assert.equal(disconnect.subarray(15).readUInt32LE(), rcGuest.ip);
        console.log('Eden hosts, Switch finds and joins, game datagrams in both directions, station leaves');

        // Use only repeated poll replies, as stock Eden does after a peer leaves.
        // Measure across the full 6.5-second expiry window for the first reply.
        let nextPoll = Date.now();
        for (let reply = 0; reply < 8; ++reply) {
            nextPoll += 880;
            await DELAY(Math.max(0, nextPoll - Date.now()));
            await recvLdn(edenHost, 0, POLL_SOURCE);
            ldn(edenHost, 1, POLL_SOURCE, edenInfo);
        }
        assert.equal((await rcGuest.search(2)).length, 6);
        ldn(edenHost, 5, rcGuestIp);
        await DELAY(100);
        rcGuest.send(0x08, GAME);
        const roomsAfterDestroy = await rcGuest.recv(0x87);
        assert.equal(roomsAfterDestroy[0], 0, 'Destroyed room still appears in console discovery');
        console.log('Repeated stock-style poll replies keep the room visible, then stop on destroy');
    } catch (error) {
        console.error(logs);
        throw error;
    } finally {
        await Promise.allSettled(clients.map(peer => peer.close()));
        relay.kill('SIGTERM');
        await relayExited;
        fs.rmSync(stateDir, { recursive: true, force: true });
    }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
