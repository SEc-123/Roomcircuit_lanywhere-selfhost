// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const Stream = require('../relay/tcp-stream-protocol');
const { create } = require('./streams');
const { TYPE, SIZE, encodeProxyInfo } = require('./protocol');

const local = 0x0a0d0017, remote = 0x0a0d0018;
const session = Buffer.alloc(32, 1), destination = { src: local, sport: 41000, dst: remote, dport: 5000, protocol: 6 };

function fixture() {
    const relay = [], client = [];
    const proxy = create({ peer: { ip: local, stream: b => { relay.push(Stream.parse(b)); return true; } },
        network: () => session, send: (id, body) => client.push({ id, body }) });
    return { relay, client, proxy };
}

function payload(route, data) {
    const header = encodeProxyInfo(route, SIZE.PROXY_DATA);
    header.writeUInt32LE(data.length, SIZE.PROXY_INFO);
    return Buffer.concat([header, data]);
}

test('translates RyuLDN-initiated TCP with stream credit and reverse bytes', () => {
    const { relay, client, proxy } = fixture();
    const route = encodeProxyInfo(destination);
    proxy.fromClient(TYPE.PROXY_CONNECT, route);
    const open = relay.shift();
    assert.equal(open.op, Stream.OP.OPEN);
    assert.deepEqual(open.payload, session);
    assert.equal(open.peer, remote);
    assert.equal(open.srcPort, destination.sport);
    assert.equal(open.dstPort, destination.dport);
    const encoded = (op, fields = {}, data) => Stream.encode(remote,
        { op, owner: 1, id: open.id, nonce: open.nonce, ...fields }, data);
    proxy.fromClient(TYPE.PROXY_DATA, payload(destination, Buffer.from('hello')));
    assert.equal(relay.length, 0, 'wait for ready');
    proxy.fromRelay(encoded(Stream.OP.OPEN_OK, { aux: 2 }));
    assert.equal(client.shift().id, TYPE.PROXY_CONNECT_REPLY);
    const first = relay.shift();
    assert.equal(first.seq, 0n);
    assert.equal(first.payload.toString(), 'he');
    proxy.fromRelay(encoded(Stream.OP.WINDOW, { seq: 2n, aux: 3 }));
    const rest = relay.shift();
    assert.equal(rest.seq, 2n);
    assert.equal(rest.payload.toString(), 'llo');
    proxy.fromRelay(encoded(Stream.OP.DATA, {}, Buffer.from('world')));
    const response = client.shift();
    assert.equal(response.id, TYPE.PROXY_DATA);
    assert.equal(response.body.readUInt32LE(0), remote);
    assert.equal(response.body.subarray(20).toString(), 'world');
    const credit = relay.shift();
    assert.equal(credit.op, Stream.OP.WINDOW);
    assert.equal(credit.seq, 5n);
    proxy.fromRelay(encoded(Stream.OP.FIN, { seq: 5n }));
    assert.equal(client.shift().id, TYPE.PROXY_DISCONNECT);
    assert.equal(relay.shift().op, Stream.OP.FIN);
});

test('translates Switch-initiated TCP and handles disconnect without echo', () => {
    const { relay, client, proxy } = fixture();
    const open = { op: Stream.OP.OPEN, owner: 0, id: 4, nonce: 5n,
        srcPort: destination.dport, dstPort: destination.sport, aux: Stream.MAX_WINDOW };
    proxy.fromRelay(Stream.encode(remote, open, session));
    const request = client.shift();
    assert.equal(request.id, TYPE.PROXY_CONNECT);
    assert.equal(request.body.readUInt32LE(0), remote);
    proxy.fromClient(TYPE.PROXY_CONNECT_REPLY, encodeProxyInfo(destination));
    assert.equal(relay.shift().op, Stream.OP.OPEN_OK);
    proxy.fromClient(TYPE.PROXY_DATA, payload(destination, Buffer.from('client data')));
    assert.equal(relay.shift().payload.toString(), 'client data');
    proxy.fromClient(TYPE.PROXY_DISCONNECT, encodeProxyInfo(destination, SIZE.PROXY_DATA));
    assert.equal(relay.shift().op, Stream.OP.RST);
    assert.equal(client.length, 0);
    proxy.fromRelay(Stream.encode(remote, { owner: 0, id: 4, nonce: 5n, op: Stream.OP.RST, aux: 6 }));
    assert.equal(client.length, 0);
    assert.equal(relay.length, 0);
});
