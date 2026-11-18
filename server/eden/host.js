// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const { spawn } = require('node:child_process');
const { timingSafeEqual } = require('node:crypto');
const { isIPv4 } = require('node:net');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { zstdCompressSync, zstdDecompressSync, constants: zstd } = require('node:zlib');

// Eden v0.2.1 and d85bad8924: Network::{RoomMember,Packet,RoomMessageTypes}.
const Message = Object.freeze({
    JOIN: 1, JOINED: 2, INFO: 3, GAME: 4, PROXY: 5, LDN: 6,
    NAME_COLLISION: 8, IP_COLLISION: 9, VERSION: 10, PASSWORD: 11, CLOSED: 12, FULL: 13,
});
const Protocol = Object.freeze({ TCP: 3, UDP: 4 });
// Historical yuzu and current Eden both advertise Room protocol v1, but yuzu
// predates the separate IP enum value. Choose the wire mapping at the listener.
const YuzuProtocol = Object.freeze({ TCP: 2, UDP: 3 });
const Ldn = Object.freeze({ SCAN: 0, SCAN_RESPONSE: 1, CONNECT: 2, SYNC: 3, DISCONNECT: 4, DESTROY: 5 });
const LDN_LENGTHS = [0, 0x480, 0x40, 0x480, 0x40, 0];
const ANY_IP = Buffer.from([255, 255, 255, 255]);
const NETWORK_VERSION = 1;
const INET = 1;
const MAX_DATA = 65507;
const PROXY_HEADER_SIZE = 21;
// A legal IPv4 UDP datagram can grow slightly when compressed. This also
// bounds the ENet worker's accepted packet size (enet-host.c).
const MAX_IPC_FRAME = PROXY_HEADER_SIZE + MAX_DATA + 1024;
const ZSTD_MAGIC = 0xfd2fb528;
const DECOMPRESS = {
    maxOutputLength: MAX_DATA,
    params: { [zstd.ZSTD_d_windowLogMax]: 17 },
};
const IPC_HEADER = 9;
const IPC = Object.freeze({ READY: 1, CONNECT: 2, RECEIVE: 3, DISCONNECT: 4, SEND: 5, DROP: 6 });
const DEFAULT_BINARY = path.join(__dirname, 'build', 'enet-host');

class Reader {
    constructor(buffer) {
        this.buffer = buffer;
        this.offset = 0;
    }

    bytes(length) {
        if (length > this.buffer.length - this.offset)
            throw new RangeError('Truncated Eden message');
        const value = this.buffer.subarray(this.offset, this.offset + length);
        this.offset += length;
        return value;
    }

    u8() { return this.bytes(1)[0]; }
    u16() { return this.bytes(2).readUInt16BE(); }
    u32() { return this.bytes(4).readUInt32BE(); }
    u64() { return this.bytes(8).readBigUInt64BE(); }

    sized(limit) {
        const length = this.u32();
        if (length > limit) throw new RangeError('Oversize Eden field');
        return this.bytes(length);
    }

    text(limit) { return this.sized(limit).toString('utf8'); }
    done() { return this.offset === this.buffer.length; }
}

function u8(value) { return Buffer.from([value]); }
function u16(value) {
    const result = Buffer.alloc(2);
    result.writeUInt16BE(value);
    return result;
}
function u32(value) {
    const result = Buffer.alloc(4);
    result.writeUInt32BE(value);
    return result;
}
function u64(value) {
    const result = Buffer.alloc(8);
    result.writeBigUInt64BE(value);
    return result;
}
function sized(value) { return Buffer.concat([u32(value.length), value]); }
function text(value) { return sized(Buffer.from(value, 'utf8')); }

function decode(buffer, wireProtocol) {
    const reader = new Reader(buffer);
    const message = reader.u8();
    let packet;
    switch (message) {
    case Message.JOIN:
        packet = {
            name: reader.text(20), requestedIp: reader.bytes(4), version: reader.u32(),
            password: reader.sized(128),
        };
        // The token field existed in earlier Yuzu forks and is still sent by Eden v0.2.1.
        if (!reader.done()) reader.sized(4096);
        break;
    case Message.LDN:
        packet = {
            kind: reader.u8(), source: reader.bytes(4), destination: reader.bytes(4),
            broadcast: reader.u8(), data: reader.sized(0x480),
        };
        if (LDN_LENGTHS[packet.kind] !== packet.data.length || packet.broadcast > 1) return null;
        break;
    case Message.PROXY: {
        const sourceFamily = reader.u8(), source = reader.bytes(4), sourcePort = reader.u16();
        const destFamily = reader.u8(), destination = reader.bytes(4), destinationPort = reader.u16();
        const wire = reader.u8(), broadcast = reader.u8();
        const protocol = wire === wireProtocol.UDP ? Protocol.UDP : wire === wireProtocol.TCP ? Protocol.TCP : null;
        const data = reader.sized(MAX_IPC_FRAME - PROXY_HEADER_SIZE);
        if (sourceFamily !== INET || destFamily !== INET || broadcast > 1 ||
            protocol === null) return null;
        packet = { source, destination, sourcePort, destinationPort, protocol, broadcast, data };
        break;
    }
    case Message.GAME:
        packet = { name: reader.text(200), id: reader.u64(), version: reader.text(32) };
        break;
    default:
        return null;
    }
    return reader.done() ? { message, ...packet } : null;
}

function isIp(value) { return Buffer.isBuffer(value) && value.length === 4; }
function isPort(value) { return Number.isInteger(value) && value >= 0 && value <= 65535; }

function encodeLdn(packet) {
    if (!isIp(packet.source) || !isIp(packet.destination) || !Buffer.isBuffer(packet.data) ||
        LDN_LENGTHS[packet.kind] !== packet.data.length)
        return null;
    return Buffer.concat([
        u8(Message.LDN), u8(packet.kind), packet.source, packet.destination,
        u8(Boolean(packet.broadcast)), sized(packet.data),
    ]);
}

function encodeProxy(packet, wireProtocol) {
    if (!isIp(packet.source) || !isIp(packet.destination) || !isPort(packet.sourcePort) ||
        !isPort(packet.destinationPort) || !Buffer.isBuffer(packet.data) || packet.data.length > MAX_DATA ||
        (packet.protocol !== Protocol.UDP && packet.protocol !== Protocol.TCP))
        return null;
    // Eden's SocketProxy expects a Zstandard frame with a known content size.
    const compressed = zstdCompressSync(packet.data, {
        pledgedSrcSize: packet.data.length,
        params: { [zstd.ZSTD_c_contentSizeFlag]: 1, [zstd.ZSTD_c_compressionLevel]: 1 },
    });
    if (compressed.length + PROXY_HEADER_SIZE > MAX_IPC_FRAME) return null;
    return Buffer.concat([
        u8(Message.PROXY), u8(INET), packet.source, u16(packet.sourcePort),
        u8(INET), packet.destination, u16(packet.destinationPort),
        u8(packet.protocol === Protocol.UDP ? wireProtocol.UDP : wireProtocol.TCP),
        u8(Boolean(packet.broadcast)), sized(compressed),
    ]);
}

function roomInfo(port, name, slots, peers) {
    const members = [...peers.values()].filter(peer => peer.ip);
    const fields = [
        u8(Message.INFO), text(name), text('RoomCircuit cross-platform multiplayer'),
        u32(slots), u16(port), text(''), text(''), u32(members.length),
    ];
    for (const peer of members) {
        fields.push(
            text(peer.name), peer.ip, text(peer.game?.name ?? ''),
            u64(peer.game?.id ?? 0n), text(peer.game?.version ?? ''),
            text(''), text(''), text(''),
        );
    }
    return Buffer.concat(fields);
}

function create({
    binary = DEFAULT_BINARY,
    host = '127.0.0.1',
    port,
    name = 'RoomCircuit',
    legacyYuzu = false,
    slots = 32,
    password = '',
    allocateIp,
    onJoin = () => {},
    onLeave = () => {},
    onLdn,
    onProxy,
    onError = () => {},
    log = () => {},
}) {
    if (!isIPv4(host) || !isPort(port) || !Number.isInteger(slots) || slots < 1 || slots > 64)
        throw new RangeError('Invalid Eden host, port or room capacity');
    if (typeof binary !== 'string' || !binary || typeof allocateIp !== 'function' ||
        typeof onLdn !== 'function' || typeof onProxy !== 'function')
        throw new TypeError('Eden transport requires a binary, an address allocator and packet handlers');
    if (typeof zstdCompressSync !== 'function' || typeof zstdDecompressSync !== 'function')
        throw new Error('Eden requires a Node.js release with Zstandard support (22.15+ or 24 LTS)');
    const wireProtocol = legacyYuzu ? YuzuProtocol : Protocol;
    const roomPassword = Buffer.from(password, 'utf8');
    if (roomPassword.length > 128) throw new RangeError('Eden room password exceeds 128 bytes');
    const peers = new Map();
    const child = spawn(binary, [host, String(port)], { stdio: ['pipe', 'pipe', 'pipe'] });
    let pending = Buffer.alloc(0), activePort = null, closing = false;
    let resolveReady, rejectReady;
    const ready = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
    const stopped = new Promise(resolve => child.once('close', resolve));

    function transmit(operation, peerId, packet = Buffer.alloc(0)) {
        if (child.stdin.destroyed || child.stdin.writableEnded ||
            child.stdin.writableLength + packet.length > 1024 * 1024)
            return false;
        const header = Buffer.alloc(IPC_HEADER);
        header[0] = operation;
        header.writeUInt32BE(peerId, 1);
        header.writeUInt32BE(packet.length, 5);
        child.stdin.write(Buffer.concat([header, packet]));
        return true;
    }

    function sendPacket(peer, packet) {
        return Boolean(packet && peer && peers.get(peer.id) === peer && transmit(IPC.SEND, peer.id, packet));
    }

    function broadcastInfo() {
        const update = roomInfo(activePort, name, slots, peers);
        for (const peer of peers.values())
            if (peer.ip && !peer.dropping) sendPacket(peer, update);
    }

    function remove(peer) {
        peers.delete(peer.id);
        if (peer.ip) onLeave(peer);
    }

    function drop(peer) {
        if (peer.dropping) return;
        peer.dropping = true;
        transmit(IPC.DROP, peer.id);
    }

    function reject(peer, message, details = Buffer.alloc(0)) {
        sendPacket(peer, Buffer.concat([u8(message), details]));
        drop(peer);
    }

    function join(peer, packet) {
        if (packet.version !== NETWORK_VERSION)
            return reject(peer, Message.VERSION, u32(NETWORK_VERSION));
        if (packet.password.length !== roomPassword.length || !timingSafeEqual(packet.password, roomPassword))
            return reject(peer, Message.PASSWORD);
        if (!/^[ a-zA-Z0-9._-]{4,20}$/.test(packet.name) ||
            [...peers.values()].some(other => other.name === packet.name && other.ip))
            return reject(peer, Message.NAME_COLLISION);
        if ([...peers.values()].filter(other => other.ip).length >= slots)
            return reject(peer, Message.FULL);
        if (!packet.requestedIp.equals(ANY_IP))
            return reject(peer, Message.IP_COLLISION);

        peer.name = packet.name;
        peer.ip = allocateIp(peer) ?? undefined;
        if (!isIp(peer.ip)) {
            peer.ip = undefined;
            return reject(peer, Message.FULL);
        }
        // Send room membership before success. Eden reads its room details as it leaves Joining.
        broadcastInfo();
        sendPacket(peer, Buffer.concat([u8(Message.JOINED), peer.ip]));
        onJoin(peer);
    }

    function receive(peer, bytes) {
        if (peer.dropping) return;
        const now = performance.now();
        if (now - peer.rateSince >= 1000) {
            peer.rateSince = now;
            peer.receivedBytes = 0;
            peer.decodedBytes = 0;
            peer.receivedFrames = 0;
            peer.controlFrames = 0;
        }
        peer.receivedBytes += bytes.length;
        if (++peer.receivedFrames > 2400 || peer.receivedBytes > 1024 * 1024) {
            drop(peer);
            return;
        }

        let packet;
        try {
            packet = decode(bytes, wireProtocol);
        } catch (error) {
            if (!(error instanceof RangeError)) throw error;
            return;
        }
        if (!packet) return;
        if (!peer.ip) {
            if (packet.message === Message.JOIN) join(peer, packet);
            return;
        }
        switch (packet.message) {
        case Message.GAME:
            if (++peer.controlFrames > 32) return;
            peer.game = packet;
            broadcastInfo();
            break;
        case Message.LDN:
            if (++peer.controlFrames > 32) return;
            packet.source = peer.ip;
            packet.broadcast = Boolean(packet.broadcast);
            onLdn(peer, packet);
            break;
        case Message.PROXY:
            packet.source = peer.ip;
            packet.broadcast = Boolean(packet.broadcast);
            if (packet.data.length < 4 || packet.data.readUInt32LE(0) !== ZSTD_MAGIC) return;
            try {
                packet.data = zstdDecompressSync(packet.data, DECOMPRESS);
            } catch {
                // An invalid or oversize frame is local to the sender.
                return;
            }
            peer.decodedBytes += packet.data.length;
            if (peer.decodedBytes > 1024 * 1024) {
                drop(peer);
                return;
            }
            onProxy(peer, packet);
            break;
        default:
            break;
        }
    }

    function dispatch(op, id, data) {
        if (op === IPC.READY && data.length === 2 && activePort === null) {
            activePort = data.readUInt16BE(0);
            resolveReady(activePort);
            return;
        }
        if (op === IPC.CONNECT && id && data.length === 4) {
            peers.set(id, {
                id, address: [...data].join('.'), timeout: performance.now() + 5000,
                rateSince: performance.now(), receivedFrames: 0, receivedBytes: 0, decodedBytes: 0,
                controlFrames: 0,
            });
            return;
        }
        const peer = peers.get(id);
        if (!peer) return;
        if (op === IPC.DISCONNECT) {
            remove(peer);
            if (peer.ip && !closing) broadcastInfo();
        } else if (op === IPC.RECEIVE && !closing) {
            receive(peer, data);
        }
    }

    child.stdout.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= IPC_HEADER) {
            const length = pending.readUInt32BE(5);
            if (length > MAX_IPC_FRAME) {
                child.kill();
                return;
            }
            if (pending.length < IPC_HEADER + length) break;
            dispatch(pending[0], pending.readUInt32BE(1), pending.subarray(IPC_HEADER, IPC_HEADER + length));
            pending = pending.subarray(IPC_HEADER + length);
        }
    });

    child.stderr.on('data', chunk => log(chunk.toString().trim()));
    child.once('error', error => rejectReady(error));
    // Exit is handled centrally. Suppress EPIPE while the worker is terminating.
    child.stdin.on('error', () => {});
    const sweep = setInterval(() => {
        const now = performance.now();
        for (const peer of peers.values())
            if (!peer.ip && peer.timeout < now) drop(peer);
    }, 1000);
    sweep.unref();

    child.once('close', (code, signal) => {
        const unexpected = !closing;
        closing = true;
        clearInterval(sweep);
        for (const peer of peers.values()) remove(peer);
        if (unexpected) {
            const error = new Error(`Eden ENet worker stopped (${signal ?? code})`);
            rejectReady(error);
            onError(error);
        }
    });

    return {
        ready,
        peers,
        sendLdn: (peer, packet) => sendPacket(peer, encodeLdn(packet)),
        sendProxy: (peer, packet) => sendPacket(peer, encodeProxy(packet, wireProtocol)),
        close() {
            if (!closing) {
                closing = true;
                rejectReady(new Error('Eden host closed'));
                for (const peer of peers.values()) {
                    if (peer.ip) sendPacket(peer, u8(Message.CLOSED));
                    drop(peer);
                }
                child.stdin.end();
                const stop = setTimeout(() => child.kill('SIGKILL'), 1500);
                stop.unref();
                stopped.then(() => clearTimeout(stop));
            }
            return stopped;
        },
    };
}

module.exports = { create, Ldn, Protocol, Message, DEFAULT_BINARY };
