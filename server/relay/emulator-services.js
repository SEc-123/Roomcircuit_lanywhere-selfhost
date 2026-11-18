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

function createEden({ bridge, host, port, binary, log = () => {} }) {
    const { create, Ldn, Protocol } = load('eden', 'host.js');
    const peers = new Map();
    const discovery = address(DISCOVERY_ADDRESS);
    const targetIp = packet => packet.destination.readUInt32BE(0);
    function packet(peer, kind, from, data = EMPTY) {
        return frontend.sendLdn(peer, { kind, source: address(from), destination: peer.ip, data });
    }
    const frontend = create({
        host, port, binary, log, onError: error => log(error.message),
        allocateIp(peer) {
            const state = { published: null, joined: null, seen: 0, lastPoll: 0, lastScan: 0, advertised: new Map() };
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
                state.lastScan = Date.now();
                state.advertised.clear();
                for (const info of member.scan(candidate => candidate.readUInt32LE(FIRST_NODE) !== member.ip)) {
                    const ip = info.readUInt32LE(FIRST_NODE);
                    if (packet(peer, Ldn.SCAN_RESPONSE, ip, info)) state.advertised.set(ip, info);
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
                if (message.broadcast || state.published || state.joined) return;
                const ip = targetIp(message);
                const info = Date.now() - state.lastScan < 30000 && state.advertised.get(ip);
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

module.exports = { createEden, createRyuLdn };
