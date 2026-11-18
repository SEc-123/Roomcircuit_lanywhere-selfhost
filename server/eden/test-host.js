// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const { randomBytes } = require('node:crypto');
const { zstdCompressSync, zstdDecompressSync } = require('node:zlib');
const { create, Ldn, Protocol } = require('./host');

const binaryDir = process.argv[2] ?? path.join(__dirname, 'build');
const ipOne = Buffer.from([10, 13, 0, 10]);
const ipTwo = Buffer.from([10, 13, 0, 11]);
const { client, uint, waitFor, anyIp } = require('./test-peer');

function checkCrowdedDiscovery() {
    const { scanEdenRooms } = require('../relay/emulator-services');
    const rooms = Array.from({ length: 101 }, (_, i) => {
        const info = Buffer.alloc(0x480);
        info.writeUInt32LE(0x0a0d0100 + i, 0x68);
        info.writeBigUInt64LE(BigInt(i + 1));
        return info;
    });
    const member = { ip: 0x0a0d1234, scan: predicate => rooms.filter(predicate).slice(0, 32) };
    const state = { cursor: 0 };
    const seen = new Set();
    for (let i = 0; i < 4; i++) {
        const results = scanEdenRooms(member, state, 98n);
        assert(results.size <= 33);
        assert(results.has(0x0a0d0100 + 97), 'the selected game should appear even on the first scan');
        for (const ip of results.keys()) seen.add(ip);
    }
    assert.equal(seen.size, rooms.length, 'an unrelated LCID beyond the first page must remain discoverable');

    // Below the scan limit a game must rediscover *every* room on each scan,
    // including when another host starts playing after the previous page.
    const lobby = rooms.slice(0, 2);
    const small = { ip: member.ip, scan: predicate => lobby.filter(predicate).slice(0, 32) };
    const browsing = { cursor: 0 };
    const allVisible = () => assert.deepEqual(new Set(scanEdenRooms(small, browsing, 0n).keys()),
        new Set(lobby.map(info => info.readUInt32LE(0x68))));
    allVisible();
    lobby.push(rooms[2]);
    allVisible();
    allVisible();
    lobby.splice(lobby.findIndex(info => info.readUInt32LE(0x68) === browsing.cursor), 1);
    allVisible();
}

async function main() {
    checkCrowdedDiscovery();
    const connected = [], departed = [], received = [];
    const room = create({
        binary: path.resolve(binaryDir, 'enet-host'), port: 0, password: 'play',
        allocateIp: peer => peer.name === 'PlayerOne' ? ipOne : ipTwo,
        onJoin: peer => connected.push(peer),
        onLeave: peer => departed.push(peer),
        onLdn: (peer, data) => received.push({ peer, data }),
        onProxy: (peer, data) => received.push({ peer, data }),
    });
    const clients = [];
    function open(port) {
        const device = client(port, binaryDir);
        clients.push(device);
        return device;
    }
    try {
        const port = await room.ready;
        assert(port > 0);
        const host = open(port), guest = open(port), badVersion = open(port), badPassword = open(port);
        await waitFor(() => clients.every(device => device.connected), 'ENet handshakes');
        host.join('PlayerOne', 'play');
        guest.join('PlayerTwo', 'play');
        badVersion.join('Mismatch', 'play', 2);
        badPassword.join('Incorrect', 'wrong');
        await waitFor(() => connected.length === 2, 'Eden room joins');
        await waitFor(() => badVersion.messages.some(packet => packet[0] === 10), 'version error');
        await waitFor(() => badPassword.messages.some(packet => packet[0] === 11), 'password error');
        const hostJoined = await waitFor(() => host.messages.find(packet => packet[0] === 2), 'host join success');
        const guestJoined = await waitFor(() => guest.messages.find(packet => packet[0] === 2), 'guest join success');
        assert.deepEqual(hostJoined.subarray(1), ipOne);
        assert.deepEqual(guestJoined.subarray(1), ipTwo);
        assert.equal(host.messages[0][0], 3);

        const untrustedSource = Buffer.from([10, 13, 0, 70]);
        host.command(5, Buffer.concat([Buffer.from([6, 0]), untrustedSource, anyIp, Buffer.from([1]), uint(0, 4)]));
        const ldn = await waitFor(() => received[0], 'game room scan');
        assert.deepEqual(ldn.data.source, ipOne);
        assert.equal(ldn.data.kind, Ldn.SCAN);
        assert(ldn.data.broadcast);
        const guestPeer = connected.find(peer => peer.name === 'PlayerTwo');
        assert(room.sendLdn(guestPeer, {
            kind: Ldn.SCAN_RESPONSE, source: ipOne, destination: ipTwo, data: Buffer.alloc(0x480),
        }));
        const response = await waitFor(() => guest.messages.find(packet => packet[0] === 6), 'LDN scan response');
        assert.equal(response[1], 1);
        assert.equal(response.readUInt32BE(11), 0x480);
        assert.equal(response.length, 15 + 0x480);

        // ENet fragments this independently framed game payload, then reassembles it at the host.
        const gamePayload = randomBytes(8192);
        const compressed = zstdCompressSync(gamePayload);
        guest.command(5, Buffer.concat([
            Buffer.from([5, 1]), ipTwo, uint(5000, 2), Buffer.from([1]), ipOne, uint(5001, 2),
            Buffer.from([4, 0]), uint(compressed.length, 4), compressed,
        ]));
        const proxy = await waitFor(() => received[1], 'game datagram');
        assert.deepEqual(proxy.data.data, gamePayload);
        assert.equal(proxy.data.protocol, Protocol.UDP);
        assert.equal(proxy.data.sourcePort, 5000);
        assert.equal(proxy.data.destinationPort, 5001);
        assert(room.sendProxy(ldn.peer, proxy.data));
        const datagram = await waitFor(() => host.messages.find(packet => packet[0] === 5), 'outbound datagram');
        assert.deepEqual(zstdDecompressSync(datagram.subarray(21)), gamePayload);
        assert(datagram[25] & 0x20, 'single-segment frame has content size');

        // Highly compressible data larger than any valid UDP payload must be rejected.
        const oversize = zstdCompressSync(Buffer.alloc(65508, 3));
        guest.command(5, Buffer.concat([
            Buffer.from([5, 1]), ipTwo, uint(5000, 2), Buffer.from([1]), ipOne, uint(5001, 2),
            Buffer.from([4, 0]), uint(oversize.length, 4), oversize,
        ]));

        // A malformed game join (short NodeInfo) must not reach RoomCircuit.
        guest.command(5, Buffer.concat([
            Buffer.from([6, 2]), ipTwo, ipOne, Buffer.from([0]), uint(1, 4), Buffer.alloc(1),
        ]));
        guest.command(6);
        await waitFor(() => departed.includes(guestPeer), 'peer departure');
        assert.equal(received.length, 2);
    } finally {
        await Promise.allSettled([room.close(), ...clients.map(device => device.close())]);
    }
    console.log('Eden room v1 and ENet conformance passed');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
