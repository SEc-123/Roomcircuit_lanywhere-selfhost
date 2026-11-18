// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { FIRST_NODE, BCAST, sameNetwork } = require('./emulator-bridge');

const DISCOVERY_ADDRESS = 0x0a0dfefe;
const EMPTY = Buffer.alloc(0);
const address = ip => {
    const buffer = Buffer.alloc(4);
    buffer.writeUInt32BE(ip);
    return buffer;
};
const load = (name, filename) => {
    const installed = path.join(__dirname, name);
    const directory = fs.existsSync(installed) ? installed : path.join(__dirname, '..', name);
    return require(path.join(directory, filename));
};

// Eden filters rooms inside the game after receiving scan replies. The scan
// request itself carries no game filter. Rotate the shared relay's bounded
// page and include exact title/LCID matches when the client supplied a title.
function scanEdenRooms(member, state, gameId) {
    const own = member.ip;
    const hostIp = info => info.readUInt32LE(FIRST_NODE);
    const pageSize = 32;
    const earlier = [];
    let after = !state.cursor;
    const page = member.scan(info => {
        const ip = hostIp(info);
        if (after) return ip !== own;
        if (ip !== own && earlier.length < pageSize) earlier.push(info);
        if (ip === state.cursor) after = true;
        return false;
    });
    // A local game clears its previous discoveries on every scan. Wrap the
    // final page in this reply so a small lobby still includes every live host.
    if (page.length < pageSize) page.push(...earlier.slice(0, pageSize - page.length));
    state.cursor = page.length ? hostIp(page[page.length - 1]) : 0;
    const preferred = gameId ? member.scan(info => hostIp(info) !== own && info.readBigUInt64LE(0) === gameId) : [];
    return new Map([...preferred, ...page].map(info => [hostIp(info), info]));
}

function createEden({ bridge, host, port, binary, legacyYuzu = false, log = () => {} }) {
    const { create, Ldn, Protocol } = load('eden', 'host.js');
    const peers = new Map();
    const discovery = address(DISCOVERY_ADDRESS);
    const targetIp = packet => packet.destination.readUInt32BE(0);
    function packet(peer, kind, from, data = EMPTY) {
        return frontend.sendLdn(peer, { kind, source: address(from), destination: peer.ip, data });
    }
    const frontend = create({
        host, port, binary, legacyYuzu, log, onError: error => log(error.message),
        allocateIp(peer) {
            const state = { published: null, joined: null, seen: 0, lastPoll: 0, cursor: 0, advertised: new Map() };
            const member = bridge.connect({
                address: peer.address, name: peer.name,
                onControl(event) {
                    switch (event.type) {
                    case 'connect':
                        packet(peer, Ldn.CONNECT, event.ip, event.node);
                        break;
                    case 'sync':
                        packet(peer, Ldn.SYNC, event.ip, event.info);
                        break;
                    case 'disconnect':
                        if (state.published && event.node) packet(peer, Ldn.DISCONNECT, event.ip, event.node);
                        if (state.joined) packet(peer, Ldn.DESTROY, state.joined.readUInt32LE(FIRST_NODE));
                        break;
                    case 'error':
                        // A rejected host update leaves its last valid publication active.
                        if (!state.joined || member.admitted()) break;
                        member.leave();
                        // fall through: clear a rejected join and allow another search.
                    case 'closed':
                        if (state.joined) packet(peer, Ldn.DESTROY, state.joined.readUInt32LE(FIRST_NODE));
                        state.joined = state.published = null;
                        break;
                    }
                },
                onData(event) {
                    frontend.sendProxy(peer, {
                        source: address(event.src), destination: address(event.dst),
                        sourcePort: event.sport, destinationPort: event.dport, protocol: Protocol.UDP,
                        broadcast: event.dst === BCAST || event.dst === 0xffffffff, data: event.payload,
                    });
                },
            });
            if (!member) return null;
            state.member = member;
            peers.set(peer.id, state);
            return address(member.ip);
        },
        onLdn(peer, message) {
            const state = peers.get(peer.id);
            if (!state) return;
            const { member } = state;
            const { data, kind } = message;
            switch (kind) {
            case Ldn.SCAN:
                if (!message.broadcast) return;
                for (const [ip, info] of scanEdenRooms(member, state, peer.game?.id)) {
                    if (!packet(peer, Ldn.SCAN_RESPONSE, ip, info)) continue;
                    state.advertised.delete(ip);
                    state.advertised.set(ip, info);
                    if (state.advertised.size > 128) state.advertised.delete(state.advertised.keys().next().value);
                }
                break;
            case Ldn.SCAN_RESPONSE:
                // Stock Eden only announces itself when asked, including when it hosts alone.
                if (!message.destination.equals(discovery) || Date.now() - state.lastPoll > 2500 || state.joined) return;
                if (state.published && !sameNetwork(data, state.published)) {
                    member.leave();
                    state.published = null;
                }
                if (member.publish(data)) { state.published = Buffer.from(data); state.seen = Date.now(); }
                break;
            case Ldn.CONNECT: {
                if (message.broadcast || state.joined) return;
                const ip = targetIp(message);
                const info = state.advertised.get(ip);
                // DestroyNetwork is silent when Eden stops hosting an empty room.
                // A station CONNECT after a scan explicitly switches roles.
                if (info && state.published) {
                    member.leave();
                    state.published = null;
                }
                // join verifies the selected session against the current host. Games
                // may keep a scan result on screen for an arbitrary amount of time.
                if (info && member.join(info, data)) state.joined = info;
                else packet(peer, Ldn.DESTROY, ip);
                break;
            }
            case Ldn.SYNC:
                if (state.published && member.sync(data)) state.seen = Date.now();
                break;
            case Ldn.DISCONNECT:
                if (!state.joined || targetIp(message) !== state.joined.readUInt32LE(FIRST_NODE)) return;
                member.leave();
                state.joined = null;
                break;
            case Ldn.DESTROY:
                if (!state.published) return;
                member.leave();
                state.published = null;
                break;
            }
        },
        onProxy(peer, packet) {
            if (packet.protocol !== Protocol.UDP) return;
            peers.get(peer.id)?.member.datagram({
                dst: packet.broadcast ? BCAST : targetIp(packet),
                sport: packet.sourcePort, dport: packet.destinationPort, payload: packet.data,
            });
        },
        onLeave(peer) {
            peers.get(peer.id)?.member.close();
            peers.delete(peer.id);
        },
    });
    const timer = setInterval(() => {
        const now = Date.now();
        for (const peer of frontend.peers.values()) {
            const state = peers.get(peer.id);
            if (!state || state.joined) continue;
            if (state.published && now - state.seen > 6500) {
                state.member.leave();
                state.published = null;
            }
            state.lastPoll = now;
            frontend.sendLdn(peer, {
                kind: Ldn.SCAN, destination: peer.ip, source: discovery, broadcast: true, data: EMPTY,
            });
        }
    }, 1000).unref();
    return { ready: frontend.ready, close() { clearInterval(timer); return frontend.close(); } };
}

function createRyuLdn(options) {
    const service = load('ryuldn', 'server.js').create(options);
    return { ready: service.listen(), close: service.close };
}

module.exports = { createEden, createRyuLdn, scanEdenRooms };
