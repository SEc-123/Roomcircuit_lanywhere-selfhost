// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const { randomBytes } = require('node:crypto');
const { existsSync } = require('node:fs');
const path = require('node:path');

// The systemd installer places relay modules beside the protocol directories.
const Stream = require(existsSync(path.join(__dirname, '../tcp-stream-protocol.js'))
    ? '../tcp-stream-protocol' : '../relay/tcp-stream-protocol');
const { TYPE, SIZE, encodeProxyInfo, proxyInfo } = require('./protocol');
const { OP, REASON, MAX_CHUNK, MAX_WINDOW } = Stream;

const MAX_FLOWS = 8;
const MAX_PENDING = 128 * 1024;

function create({ peer, network, send }) {
    const tuples = new Map(), identities = new Map();
    const nonce = randomBytes(8).readBigUInt64LE() || 1n;
    let nextId = 0, pendingBytes = 0;
    const tuple = (remote, localPort, remotePort) => [remote, localPort, remotePort].join(':');
    const identity = (remote, owner, id, n) => [remote, owner, id, n].join(':');
    function emit(flow, op, fields = {}, payload) {
        return peer.stream(Stream.encode(flow.remote, { op, owner: flow.owner, id: flow.id,
            nonce: flow.nonce, ...fields }, payload));
    }
    function clientRoute(flow, protocol = 6) {
        return { src: flow.remote, sport: flow.remotePort, dst: peer.ip,
            dport: flow.localPort, protocol };
    }
    function clientClose(route) {
        const body = encodeProxyInfo(route, SIZE.PROXY_DATA);
        send(TYPE.PROXY_DISCONNECT, body);
    }
    function add(remote, localPort, remotePort, owner, id, n) {
        const t = tuple(remote, localPort, remotePort);
        if (tuples.has(t) || identities.size >= MAX_FLOWS) return null;
        const i = identity(remote, owner, id, n);
        const f = { remote, localPort, remotePort, owner, id, nonce: n, tuple: t, identity: i,
            tx: 0n, rx: 0n, consumed: 0n, credit: 0, open: false, queued: [], bytes: 0,
            closing: false, finSent: false, remoteFin: false };
        tuples.set(t, f);
        identities.set(i, f);
        return f;
    }
    function remove(f) {
        pendingBytes -= f.bytes;
        if (tuples.get(f.tuple) === f) tuples.delete(f.tuple);
        identities.delete(f.identity);
    }
    function reset(f, reason, notify = true) {
        remove(f);
        emit(f, OP.RST, { aux: reason });
        if (notify && !f.closing) clientClose(clientRoute(f));
    }
    function pump(f) {
        while (f.open && f.queued.length) {
            const credit = f.consumed + BigInt(f.credit) - f.tx;
            if (credit <= 0n) return;
            const head = f.queued[0], length = Math.min(head.length, MAX_CHUNK, Number(credit));
            if (!emit(f, OP.DATA, { seq: f.tx }, head.subarray(0, length))) {
                reset(f, REASON.BACKPRESSURE);
                return;
            }
            f.tx += BigInt(length);
            pendingBytes -= length;
            f.bytes -= length;
            if (length === head.length) f.queued.shift();
            else f.queued[0] = head.subarray(length);
        }
        if (f.closing && f.open && !f.finSent) {
            if (!emit(f, OP.FIN, { seq: f.tx })) return reset(f, REASON.BACKPRESSURE, false);
            f.finSent = true;
        }
        if (f.finSent && f.remoteFin && f.consumed === f.tx) remove(f);
    }
    function fromClient(type, body) {
        const current = network();
        if (!current || body.length < SIZE.PROXY_INFO) return;
        const r = proxyInfo(body);
        if (r.protocol !== 6 || (r.src !== peer.ip && r.src !== 0) || !r.sport || !r.dport ||
            r.dst === peer.ip || (r.dst & 0xffff0000) !== 0x0a0d0000) return;
        let f = tuples.get(tuple(r.dst, r.sport, r.dport));
        if (type === TYPE.PROXY_CONNECT && body.length === SIZE.PROXY_INFO) {
            if (f) return;
            f = add(r.dst, r.sport, r.dport, 0, ++nextId, nonce);
            if (!f) return clientClose({ src: r.dst, sport: r.dport, dst: peer.ip, dport: r.sport, protocol: 6 });
            if (!emit(f, OP.OPEN, { srcPort: r.sport, dstPort: r.dport, aux: MAX_WINDOW },
                current.subarray(0, 32))) reset(f, REASON.REFUSED);
            return;
        }
        if (!f) return;
        if (type === TYPE.PROXY_CONNECT_REPLY && body.length === SIZE.PROXY_INFO && f.owner === 1 && !f.open) {
            if (!emit(f, OP.OPEN_OK, { aux: MAX_WINDOW })) return reset(f, REASON.REFUSED);
            f.open = true;
            pump(f);
        } else if (type === TYPE.PROXY_DISCONNECT && body.length === SIZE.PROXY_DATA) {
            if (!f.open || body.readInt32LE(SIZE.PROXY_INFO) !== 0) return reset(f, REASON.CLOSED, false);
            f.closing = true;
            tuples.delete(f.tuple);
            pump(f);
        } else if (type === TYPE.PROXY_DATA && body.length >= SIZE.PROXY_DATA &&
            body.readUInt32LE(SIZE.PROXY_INFO) === body.length - SIZE.PROXY_DATA) {
            const payload = body.subarray(SIZE.PROXY_DATA);
            if (!payload.length) return;
            if (pendingBytes + payload.length > MAX_PENDING) return reset(f, REASON.BACKPRESSURE);
            f.queued.push(payload);
            f.bytes += payload.length;
            pendingBytes += payload.length;
            pump(f);
        }
    }
    function fromRelay(body) {
        const h = Stream.parse(body), current = network();
        if (!h || !current) return;
        let f = identities.get(identity(h.peer, 1 - h.owner, h.id, h.nonce));
        if (h.op === OP.OPEN) {
            if (!h.payload.equals(current.subarray(0, 32))) return;
            f = add(h.peer, h.dstPort, h.srcPort, 1, h.id, h.nonce);
            if (!f) {
                peer.stream(Stream.encode(h.peer, { op: OP.RST, owner: 1, id: h.id,
                    nonce: h.nonce, aux: REASON.CAPACITY }));
                return;
            }
            f.credit = h.aux;
            send(TYPE.PROXY_CONNECT, encodeProxyInfo(clientRoute(f)));
            return;
        }
        if (!f) return;
        if ((h.op === OP.OPEN_OK || h.op === OP.LEGACY_READY) && f.owner === 0 && !f.open) {
            f.open = true;
            f.credit = h.aux;
            send(TYPE.PROXY_CONNECT_REPLY, encodeProxyInfo(clientRoute(f)));
            pump(f);
        } else if (h.op === OP.RST) {
            remove(f);
            if (!f.closing) clientClose(clientRoute(f));
        } else if (h.op === OP.DATA && f.open && h.seq === f.rx) {
            const head = encodeProxyInfo(clientRoute(f), SIZE.PROXY_DATA);
            head.writeUInt32LE(h.payload.length, SIZE.PROXY_INFO);
            if (!f.closing) send(TYPE.PROXY_DATA, Buffer.concat([head, h.payload]));
            f.rx += BigInt(h.payload.length);
            if (!emit(f, OP.WINDOW, { seq: f.rx, aux: MAX_WINDOW })) reset(f, REASON.BACKPRESSURE);
        } else if (h.op === OP.WINDOW && h.seq >= f.consumed && h.seq <= f.tx) {
            f.consumed = h.seq;
            f.credit = h.aux;
            pump(f);
        } else if (h.op === OP.FIN && f.open && h.seq === f.rx) {
            // RyuLDN closes both directions. Preserve all writes already accepted
            // from its client before advertising the final native byte offset.
            if (!f.closing) clientClose(clientRoute(f));
            f.closing = f.remoteFin = true;
            if (tuples.get(f.tuple) === f) tuples.delete(f.tuple);
            pump(f);
        } else reset(f, REASON.PROTOCOL);
    }
    return { fromClient, fromRelay, clear() { tuples.clear(); identities.clear(); pendingBytes = 0; } };
}

module.exports = { create };
