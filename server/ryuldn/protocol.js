// SPDX-License-Identifier: GPL-2.0-only
'use strict';

// Wire layouts from Ryubing/LdnServer ba32bcf0878184b3f30b3db4e2d3888a08aa17dc.
// Its C# structures use little-endian integers, including logical IPv4 addresses.
const { randomBytes } = require('node:crypto');

const TYPE = Object.freeze({
    INITIALIZE: 0, PASSPHRASE: 1, CREATE: 2, CREATE_PRIVATE: 3,
    EXTERNAL_PROXY_STATE: 6, SYNC: 7, REJECT: 8, REJECT_REPLY: 9,
    SCAN: 10, SCAN_REPLY: 11, SCAN_END: 12, CONNECT: 13, CONNECT_PRIVATE: 14,
    CONNECTED: 15, DISCONNECT: 16, PROXY_CONFIG: 17, PROXY_CONNECT: 18,
    PROXY_CONNECT_REPLY: 19, PROXY_DATA: 20, PROXY_DISCONNECT: 21,
    ACCEPT_POLICY: 22, ADVERTISE: 23, PING: 254, ERROR: 255
});
const ERROR = Object.freeze({
    PORT_UNREACHABLE: 1, FULL: 2, VERSION_LOW: 3, VERSION_HIGH: 4,
    CONNECT_FAILURE: 5, NOT_FOUND: 6, TIMEOUT: 7, REJECTED: 8, REJECT_FAILED: 9,
    UNKNOWN: -1
});

const SIZE = Object.freeze({ HEADER: 12, NETWORK: 0x480, NODE: 0x40, SCAN: 0x60,
    CREATE: 0xbc, CONNECT: 0x4fc, ADVERTISE: 384, PROXY_INFO: 16, PROXY_DATA: 20 });
const OFFSET = Object.freeze({ BSSID: 0x20, SCENE: 10, SESSION: 0x10, SSID: 0x26,
    CHANNEL: 0x48, LINK_LEVEL: 0x4a, NETWORK_TYPE: 0x4b,
    SECURITY_MODE: 0x60, ACCEPT_POLICY: 0x62, MAX_NODES: 0x66,
    NODE_COUNT: 0x67, NODES: 0x68, ADVERTISE_SIZE: 0x26a, ADVERTISE: 0x26c,
    NODE_MAC: 4, NODE_INDEX: 10, NODE_ONLINE: 11, NODE_NAME: 12, NODE_VERSION: 46 });
const MAX_NODES = 8;
const MAX_FRAME = 131071;
const SUBNET = 0xffff0000;
const BCAST = 0x0a0dffff;
const MASKED_BROADCAST = 0xc0a800ff;
const MAGIC = Buffer.from('RLDN');

function frame(type, body = Buffer.alloc(0)) {
    const header = Buffer.alloc(SIZE.HEADER);
    MAGIC.copy(header);
    header[4] = type;
    header[5] = 1;
    header.writeUInt32LE(body.length, 8);
    return Buffer.concat([header, body]);
}

function streamDecoder(onFrame) {
    const header = Buffer.alloc(SIZE.HEADER);
    let offset = 0, payload = null;
    return function feed(chunk) {
        while (chunk.length) {
            const target = payload ?? header;
            const copied = chunk.copy(target, offset);
            offset += copied;
            chunk = chunk.subarray(copied);
            if (offset !== target.length) continue;
            offset = 0;
            if (payload !== null) {
                const complete = payload;
                payload = null;
                if (onFrame(header[4], complete) === false) return;
                continue;
            }
            const length = header.readUInt32LE(8);
            if (!header.subarray(0, 4).equals(MAGIC) || header[5] !== 1 || length > MAX_FRAME - SIZE.HEADER)
                throw new Error('Invalid RyuLDN frame');
            if (length) payload = Buffer.alloc(length);
            else if (onFrame(header[4], Buffer.alloc(0)) === false) return;
        }
    };
}

function node(info, index) {
    const start = OFFSET.NODES + index * SIZE.NODE;
    return info.subarray(start, start + SIZE.NODE);
}

function setAdvertise(info, bytes) {
    bytes.copy(info, OFFSET.ADVERTISE);
    info.fill(0, OFFSET.ADVERTISE + bytes.length, OFFSET.ADVERTISE + SIZE.ADVERTISE);
    info.writeUInt16LE(bytes.length, OFFSET.ADVERTISE_SIZE);
}

function nodeInfo({ ip, mac, name, version }) {
    const info = Buffer.alloc(SIZE.NODE);
    info.writeUInt32LE(ip, 0);
    mac.copy(info, OFFSET.NODE_MAC);
    info[OFFSET.NODE_ONLINE] = 1;
    name.copy(info, OFFSET.NODE_NAME, 0, 32);
    info.writeUInt16LE(version, OFFSET.NODE_VERSION);
    return info;
}

function hostInfo(body, ownNode) {
    const info = Buffer.alloc(SIZE.NETWORK);
    body.copy(info, 0, 0x74, 0x84);
    randomBytes(16).copy(info, OFFSET.SESSION);
    ownNode.copy(info, OFFSET.NODES);
    ownNode.copy(info, OFFSET.BSSID, OFFSET.NODE_MAC, OFFSET.NODE_MAC + 6);
    const ssid = Buffer.from('12345678123456781234567812345678');
    info[OFFSET.SSID] = ssid.length;
    ssid.copy(info, OFFSET.SSID + 1);
    body.copy(info, OFFSET.CHANNEL, 0x84, 0x86);
    info[OFFSET.LINK_LEVEL] = 3;
    info[OFFSET.NETWORK_TYPE] = 2;
    body.copy(info, OFFSET.SECURITY_MODE, 0, 2);
    info[OFFSET.MAX_NODES] = body[0x86];
    info[OFFSET.NODE_COUNT] = 1;
    setAdvertise(info, body.subarray(SIZE.CREATE));
    return info;
}

function scanMatches(info, query) {
    const flags = query[0x5c];
    const matches = (a, b, n) => info.subarray(a, a + n).equals(query.subarray(b, b + n));
    const type = query.readUInt32LE(0x20);
    return (!(flags & 1) || matches(0, 0, 8)) &&
        (!(flags & 2) || matches(OFFSET.SESSION, OFFSET.SESSION, 16)) &&
        (!(flags & 4) || type === 3 || type === info[OFFSET.NETWORK_TYPE]) &&
        (!(flags & 8) || matches(OFFSET.BSSID, 0x24, 6)) &&
        (!(flags & 16) || (query[0x2a] <= 32 && matches(OFFSET.SSID, 0x2a, query[0x2a] + 1))) &&
        (!(flags & 32) || matches(OFFSET.SCENE, OFFSET.SCENE, 2));
}

function sameSession(a, b) {
    return a.subarray(0, 32).equals(b.subarray(0, 32)) &&
        a.subarray(OFFSET.BSSID, OFFSET.BSSID + 6).equals(b.subarray(OFFSET.BSSID, OFFSET.BSSID + 6));
}

function proxyInfo(body) {
    return { src: body.readUInt32LE(0), sport: body.readUInt16LE(4), dst: body.readUInt32LE(6),
        dport: body.readUInt16LE(10), protocol: body.readUInt32LE(12) };
}

function encodeProxyInfo({ src, sport, dst, dport, protocol }, size = SIZE.PROXY_INFO) {
    const body = Buffer.alloc(size);
    body.writeUInt32LE(src, 0);
    body.writeUInt16LE(sport, 4);
    body.writeUInt32LE(dst, 6);
    body.writeUInt16LE(dport, 10);
    body.writeUInt32LE(protocol, 12);
    return body;
}

module.exports = {
    TYPE, ERROR, SIZE, OFFSET, MAX_NODES, SUBNET, BCAST, MASKED_BROADCAST,
    frame, streamDecoder, node, nodeInfo, setAdvertise, hostInfo, scanMatches, sameSession,
    proxyInfo, encodeProxyInfo
};
