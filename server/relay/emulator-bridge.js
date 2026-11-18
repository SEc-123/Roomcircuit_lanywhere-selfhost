// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const net = require('node:net');
const Stream = require('./tcp-stream-protocol');

const T = {
    CREATE: 0x01, JOIN: 0x02, LEAVE: 0x03, RELAY: 0x04, PUBLISH: 0x07,
    MEMBER_LEFT: 0x83, RELAY_IN: 0x84, ERROR: 0x85,
};
const LDN = { SCAN: 1, SCAN_RESPONSE: 2, CONNECT: 3, SYNC: 4, DISCONNECT: 5 };
const NET = 0x0a0d0000, MASK = 0xffff0000, BCAST = 0x0a0dffff;
const NETWORK_SIZE = 0x480, NODE_SIZE = 0x40, FIRST_NODE = 0x68;
const NODE_MAX = 0x66, NODE_COUNT = 0x67, ACCEPT_POLICY = 0x62;
const MAX_NODES = 8, ADVERTISE_SIZE = 0x26a;
const MAX_HOST_RESULTS = 32;

function networkIp(value) {
    return Number.isInteger(value) && value > NET && value < BCAST && (value & MASK) === NET;
}

function networkInfo(info) {
    return Buffer.isBuffer(info) && info.length === NETWORK_SIZE &&
        info.readBigUInt64LE(0) !== 0n && info.readBigUInt64LE(0) !== 0xffffffffffffffffn &&
        info[NODE_MAX] >= 1 && info[NODE_MAX] <= MAX_NODES &&
        info[NODE_COUNT] >= 1 && info[NODE_COUNT] <= info[NODE_MAX] &&
        info[ACCEPT_POLICY] <= 3 && info.readUInt16LE(ADVERTISE_SIZE) <= 384 &&
        info[FIRST_NODE + 11] === 1 && networkIp(info.readUInt32LE(FIRST_NODE));
}

function sameNetwork(a, b) {
    return a.subarray(0, 0x26).equals(b.subarray(0, 0x26));
}

function create({ rooms, frame, forwardData, isOpen, allocateIp, memberAt, paused = () => false,
    count = () => {}, maxPeers = 1024, maxPerAddress = 12, maxControlPerSecond = 100,
    maxGamePayload = 65507, log = () => {} }) {
    const peers = new Map(), byAddress = new Map();
    let closed = false;

    function connect({ address, name = '', onControl = () => {}, onData = () => {}, onStream }) {
        if (closed || paused() || !net.isIP(address) || peers.size >= maxPeers ||
            (byAddress.get(address) || 0) >= maxPerAddress) return null;
        const ip = allocateIp();
        if (!networkIp(ip) || peers.has(ip) || memberAt(ip)) return null;
        const nickname = Buffer.alloc(32);
        nickname.write(String(name), 0, nickname.length, 'utf8');
        const features = 1 | (onStream ? Stream.MEMBER_FEATURE : 0);
        let generation = 0;
        let second = -1, controls = 0;
        let session = null;
        const stations = new Map();

        function budget() {
            if (socket.destroyed || paused()) return false;
            const now = Math.floor(Date.now() / 1000);
            if (second !== now) { second = now; controls = 0; }
            return ++controls <= maxControlPerSecond;
        }

        function notify(callback, event, member = socket.member) {
            const epoch = generation;
            // A frontend can leave a room in response to an event. Deliver only
            // after the relay finishes changing the membership and publication.
            queueMicrotask(() => {
                if (closed || socket.destroyed || epoch !== generation || member !== socket.member) return;
                try { callback(event); }
                catch (error) { log(`emulator callback failed: ${error.message}`); peer.close(); }
            });
            return true;
        }

        function control(dest, type, payload = Buffer.alloc(0)) {
            if (socket.member) frame(socket, T.RELAY, Buffer.concat([Buffer.from([dest, type]), payload]));
        }

        const socket = {
            virtual: true, emulatorBridge: true, destroyed: false, member: null, ip13: ip, remoteAddress: address,
            deliver(type, body) {
                const member = socket.member;
                if (type === Stream.RESPONSE && onStream) return notify(onStream, Buffer.from(body), member);
                if (type === T.ERROR) return notify(onControl, { type: 'error', code: body[0] }, member);
                if (!member) return true;
                if (type === T.MEMBER_LEFT && body.length === 1) {
                    const departed = stations.get(body[0]);
                    if (departed) {
                        stations.delete(body[0]);
                        return notify(onControl, { type: 'disconnect',
                            ip: departed.readUInt32LE(0), node: departed }, member);
                    }
                    // A seat may disappear before a CONNECT reaches this host.
                    return true;
                }
                if (type !== T.RELAY_IN || body.length < 2) return true;
                const index = body[0], kind = body[1], payload = body.subarray(2);
                const remote = member.room.members[index];
                if (!remote) return true;
                if (kind === LDN.CONNECT && member.index === 0 && payload.length === NODE_SIZE) {
                    if (stations.has(index)) {
                        const publication = member.room.published;
                        if (publication?.connected.has(remote)) control(index, LDN.SYNC, publication.info);
                        return true;
                    }
                    const node = Buffer.from(payload);
                    stations.set(index, node);
                    return notify(onControl, { type: 'connect', ip: remote.ip, node: Buffer.from(node) }, member);
                }
                if (kind === LDN.SCAN && member.index === 0 && member.room.published) {
                    control(index, LDN.SCAN_RESPONSE, member.room.published.info);
                    return true;
                }
                if ((kind === LDN.SYNC || kind === LDN.SCAN_RESPONSE) && index === 0 &&
                    networkInfo(payload) && session && sameNetwork(payload, session)) {
                    // A fresh guest can receive a delayed host-only snapshot while
                    // the game processes its CONNECT. Only the host publication and
                    // a matching connected row together confirm admission.
                    if (kind === LDN.SYNC && (!member.room.published?.connected.has(member) ||
                        !member.room.members.some((_, i) => {
                            const row = FIRST_NODE + i * NODE_SIZE;
                            return payload[row + 11] === 1 && payload.readUInt32LE(row) === ip;
                        }))) return true;
                    return notify(onControl, { type: kind === LDN.SYNC ? 'sync' : 'scan',
                        ip: remote.ip, info: Buffer.from(payload) }, member);
                }
                if (kind === LDN.DISCONNECT) {
                    const node = stations.get(index);
                    if (member.index === 0) stations.delete(index);
                    return notify(onControl, { type: 'disconnect', ip: remote.ip, node }, member);
                }
                return true;
            },
            data(from, dst, data) {
                const payload = Buffer.from(data.subarray(4));
                count(payload.length + 28);
                return notify(onData, { src: from.ip, dst, sport: data.readUInt16LE(0),
                    dport: data.readUInt16LE(2), payload });
            },
            onLeave(_reason, member) {
                if (member.socket !== socket) return;
                generation++;
                session = null;
                stations.clear();
                notify(onControl, { type: 'closed' }, null);
            },
        };

        const peer = {
            ip,
            setName(value) {
                if (socket.member) return false;
                nickname.fill(0);
                nickname.write(String(value), 0, nickname.length, 'utf8');
                return true;
            },
            scan(predicate = () => true) {
                if (!budget()) return [];
                const out = [];
                for (const room of rooms.values()) {
                    if (!room.net13 || !isOpen(room) || !networkInfo(room.published?.info)) continue;
                    const info = Buffer.from(room.published.info);
                    if (predicate(info)) out.push(info);
                    if (out.length === MAX_HOST_RESULTS) break;
                }
                return out;
            },
            publish(info) {
                if (!budget() || !networkInfo(info) || info.readUInt32LE(FIRST_NODE) !== ip) return null;
                if (socket.member) {
                    if (socket.member.index !== 0) return null;
                    if (session && !sameNetwork(info, session)) return null;
                } else {
                    if (info[NODE_COUNT] !== 1) return null;
                    generation++;
                    frame(socket, T.CREATE, Buffer.concat([info.subarray(0, 8), nickname, Buffer.from([features])]));
                }
                const member = socket.member;
                if (!member || member.index !== 0) return null;
                const previous = member.room.published;
                frame(socket, T.PUBLISH, info);
                const publication = member.room.published;
                if (!publication || previous === publication) {
                    if (!previous) peer.leave();
                    return null;
                }
                session = Buffer.from(publication.info.subarray(0, 0x26));
                for (const station of publication.connected) {
                    if (station !== member) control(station.index, LDN.SYNC, publication.info);
                }
                return Buffer.from(publication.info);
            },
            sync(info) { return Boolean(socket.member && peer.publish(info)); },
            join(info, node) {
                if (!budget() || socket.member || !networkInfo(info) ||
                    !Buffer.isBuffer(node) || node.length !== NODE_SIZE ||
                    node.readUInt32LE(0) !== ip || node[11] !== 1 ||
                    node.readUInt16LE(46) !== info.readUInt16LE(FIRST_NODE + 46)) return false;
                const host = memberAt(info.readUInt32LE(FIRST_NODE));
                const room = host?.room;
                if (!room?.net13 || host.index !== 0 || rooms.get(room.code) !== room ||
                    !isOpen(room) || !sameNetwork(room.published.info, info)) return false;
                generation++;
                session = Buffer.from(info.subarray(0, 0x26));
                frame(socket, T.JOIN, Buffer.concat([Buffer.from(room.code, 'ascii'), nickname,
                    room.published.info.subarray(0, 8), Buffer.from([features])]));
                if (!socket.member) { session = null; return false; }
                control(0, LDN.CONNECT, node);
                return true;
            },
            leave() {
                if (socket.member) frame(socket, T.LEAVE, Buffer.alloc(0));
                generation++;
                session = null;
                stations.clear();
            },
            kick(remoteIp) {
                const member = socket.member;
                if (!budget() || member?.index !== 0) return false;
                const remote = member.room.members.find(m => m?.ip === remoteIp);
                if (!remote || !remote.index) return false;
                control(remote.index, LDN.DISCONNECT);
                return true;
            },
            close() {
                if (socket.destroyed) return;
                peer.leave();
                socket.destroyed = true;
                peers.delete(ip);
                const count = byAddress.get(address);
                if (count <= 1) byAddress.delete(address);
                else byAddress.set(address, count - 1);
            },
            datagram({ dst, sport, dport, payload }) {
                const member = socket.member;
                if (closed || paused() || !member?.room.published?.connected.has(member) ||
                    !(dst === BCAST || dst === 0xffffffff || networkIp(dst)) ||
                    !Number.isInteger(sport) || sport < 1 || sport > 65535 ||
                    !Number.isInteger(dport) || dport < 1 || dport > 65535 ||
                    !Buffer.isBuffer(payload) || payload.length > maxGamePayload) return false;
                if (dst !== BCAST && dst !== 0xffffffff &&
                    !member.room.members.some(other => other !== member && other?.ip === dst &&
                        member.room.published.connected.has(other))) return false;
                const ports = Buffer.alloc(4);
                ports.writeUInt16LE(sport, 0); ports.writeUInt16LE(dport, 2);
                forwardData(member, dst, Buffer.concat([ports, payload]));
                return true;
            },
            stream(body) {
                if (!onStream || paused() || !socket.member || !Stream.parse(body)) return false;
                frame(socket, Stream.COMMAND, body);
                return true;
            },
            admitted() {
                return Boolean(socket.member?.room.published?.connected.has(socket.member));
            },
        };
        peers.set(ip, peer);
        byAddress.set(address, (byAddress.get(address) || 0) + 1);
        return peer;
    }

    return {
        connect,
        taken: ip => peers.has(ip),
        stats: () => ({ peers: peers.size }),
        close() {
            closed = true;
            for (const peer of peers.values()) peer.close();
        },
    };
}

module.exports = { create, networkInfo, networkIp, sameNetwork, NETWORK_SIZE, NODE_SIZE,
    FIRST_NODE, NODE_MAX, NODE_COUNT, BCAST };
