// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const net = require('node:net');
const streams = require('./streams');
const { randomBytes } = require('node:crypto');
const {
    TYPE, ERROR, SIZE, OFFSET, MAX_NODES, SUBNET, BCAST, MASKED_BROADCAST, frame, streamDecoder,
    node, nodeInfo, setAdvertise, hostInfo, scanMatches, sameSession, proxyInfo, encodeProxyInfo
} = require('./protocol');

const MAX_WRITE_QUEUE = 256 * 1024;
const MAX_DATAGRAM = 65507;
const MAX_SCANS_PER_WINDOW = 4;
const MAX_SCAN_RESULTS = 64;
const MAX_FRAMES_PER_WINDOW = 4096;
const MAX_BYTES_PER_WINDOW = 4 * 1024 * 1024;
const WINDOW_MS = 1000;

function create({ bridge, host = '127.0.0.1', port = 30456, maxConnections = 256,
    maxConnectionsPerIp = 12, joinTimeoutMs = 10000, log = () => {} }) {
    const clients = new Set();
    const connectionsByIp = new Map();
    // Stock Ryujinx sends its previous Initialize ID and MAC after idle
    // disconnects. Games may keep the MAC in scan filters and saved sessions.
    const previousIdentities = new Map();
    const rememberMs = 15 * 60 * 1000;
    function rememberIdentity(id, mac) {
        const now = Date.now();
        for (const [key, entry] of previousIdentities) {
            if (entry.expires > now && previousIdentities.size < maxConnections * 8) break;
            previousIdentities.delete(key);
        }
        previousIdentities.set(id.toString('hex'), { mac, expires: now + rememberMs });
    }

    const server = net.createServer(socket => {
        const address = socket.remoteAddress;
        const count = connectionsByIp.get(address) ?? 0;
        if (clients.size >= maxConnections || count >= maxConnectionsPerIp) return socket.destroy();
        connectionsByIp.set(address, count + 1);
        clients.add(socket);
        socket.setNoDelay(true);
        socket.setKeepAlive(true, 30000);
        socket.setTimeout(120000);
        socket.on('timeout', () => socket.destroy());
        socket.on('error', () => socket.destroy());

        let peer, streamProxy, id, mac, info, state = 'idle', timer;
        let publicNetwork = true;
        let windowStart = Date.now(), frames = 0, bytes = 0, scans = 0;

        function send(type, body) {
            if (socket.destroyed) return;
            if (socket.writableLength + SIZE.HEADER + (body?.length ?? 0) > MAX_WRITE_QUEUE) {
                socket.destroy();
                return;
            }
            socket.write(frame(type, body));
        }
        function failure(code) {
            const body = Buffer.alloc(4);
            body.writeInt32LE(code);
            send(TYPE.ERROR, body);
        }
        function leave(close = false) {
            clearTimeout(timer);
            streamProxy?.clear();
            state = 'idle';
            info = null;
            if (close) peer?.close();
            else peer?.leave();
        }
        function connected(network) {
            clearTimeout(timer);
            info = network;
            const config = Buffer.alloc(8);
            config.writeUInt32LE(peer.ip, 0);
            config.writeUInt32LE(SUBNET, 4);
            send(TYPE.PROXY_CONFIG, config);
            send(TYPE.CONNECTED, info);
        }
        function control(event) {
            if (state === 'idle') return;
            const joining = state === 'joining';
            const hostIp = info?.readUInt32LE(OFFSET.NODES);
            const rejectedByHost = state !== 'host' && event.type === 'disconnect' && event.ip === hostIp;
            if (event.type === 'closed' || (event.type === 'error' && joining) || rejectedByHost) {
                leave();
                if (joining) failure(ERROR.REJECTED);
                else send(TYPE.DISCONNECT, Buffer.alloc(4));
                return;
            }
            if (event.type === 'sync' && (state === 'joining' || state === 'guest')) {
                if (state === 'joining') {
                    state = 'guest';
                    connected(event.info);
                } else {
                    info = event.info;
                    send(TYPE.SYNC, info);
                }
                return;
            }
            if (state !== 'host') return;
            const next = Buffer.from(info);
            if (event.type === 'connect') {
                const version = event.node.readUInt16LE(OFFSET.NODE_VERSION);
                if (info[OFFSET.ACCEPT_POLICY] !== 0 ||
                    version !== node(info, 0).readUInt16LE(OFFSET.NODE_VERSION)) return void peer.kick(event.ip);
                let slot = 1;
                for (; slot < info[OFFSET.MAX_NODES]; slot++) {
                    const existing = node(info, slot);
                    if (!existing[OFFSET.NODE_ONLINE] || existing.readUInt32LE(0) === event.ip) break;
                }
                if (slot >= info[OFFSET.MAX_NODES]) return void peer.kick(event.ip);
                const row = node(next, slot);
                if (!row[OFFSET.NODE_ONLINE]) next[OFFSET.NODE_COUNT]++;
                event.node.copy(row);
                row.writeUInt32LE(event.ip);
                row[OFFSET.NODE_INDEX] = slot;
                row[OFFSET.NODE_ONLINE] = 1;
            } else if (event.type === 'disconnect') {
                let removed = false;
                for (let i = 1; i < MAX_NODES; i++) {
                    const row = node(next, i);
                    if (!row[OFFSET.NODE_ONLINE] || row.readUInt32LE(0) !== event.ip) continue;
                    row.fill(0);
                    next[OFFSET.NODE_COUNT]--;
                    removed = true;
                    break;
                }
                if (!removed) return;
            } else return;
            if (peer.sync(next)) {
                info = next;
                send(TYPE.SYNC, info);
            }
        }
        function datagram({ src, dst, sport, dport, payload }) {
            if (state === 'idle' || state === 'joining') return;
            const head = encodeProxyInfo({ src, dst, sport, dport, protocol: 17 }, SIZE.PROXY_DATA);
            head.writeUInt32LE(payload.length, SIZE.PROXY_INFO);
            send(TYPE.PROXY_DATA, Buffer.concat([head, payload]));
        }
        function initialize(body) {
            id = randomBytes(16);
            const key = body.subarray(0, 16).toString('hex');
            const previous = previousIdentities.get(key);
            if (previous && previous.expires > Date.now() && previous.mac.equals(body.subarray(16))) {
                mac = previous.mac;
                previousIdentities.delete(key);
            } else {
                mac = randomBytes(6);
                mac[0] = (mac[0] | 2) & 0xfe;
            }
            peer = bridge.connect({ address, name: 'Ryujinx', onControl: control, onData: datagram,
                onStream: bytes => streamProxy.fromRelay(bytes) });
            if (!peer) return socket.destroy();
            streamProxy = streams.create({ peer, network: () => peer.admitted() ? info : null, send });
            send(TYPE.INITIALIZE, Buffer.concat([id, mac]));
        }
        function scan(query) {
            if (++scans > MAX_SCANS_PER_WINDOW) return send(TYPE.SCAN_END);
            if (publicNetwork) {
                const results = peer.scan(candidate => (!info || !sameSession(info, candidate)) &&
                    scanMatches(candidate, query));
                for (const result of results.slice(0, MAX_SCAN_RESULTS)) send(TYPE.SCAN_REPLY, result);
            }
            send(TYPE.SCAN_END);
        }
        function hostGame(body) {
            if (state !== 'idle' || !publicNetwork || body[0x86] < 1 || body[0x86] > MAX_NODES ||
                body.readUInt16LE(0) > 2 || body.readUInt16LE(2) > 64 || body.readBigUInt64LE(0x74) === 0n)
                return failure(ERROR.UNKNOWN);
            const ownNode = nodeInfo({ ip: peer.ip, mac, name: body.subarray(0x44, 0x65),
                version: body.readUInt16LE(0x88) });
            const network = hostInfo(body, ownNode);
            peer.setName(ownNode.subarray(OFFSET.NODE_NAME, OFFSET.NODE_NAME + 32).toString('utf8').split('\0')[0]);
            state = 'hosting';
            info = peer.publish(network);
            if (!info) {
                leave();
                return failure(ERROR.UNKNOWN);
            }
            // This bridge is the master proxy. It never connects to the advertised external endpoint.
            if (body.readUInt16LE(0xb8)) failure(ERROR.PORT_UNREACHABLE);
            state = 'host';
            connected(info);
        }
        function join(body) {
            if (state !== 'idle' || !publicNetwork) return failure(ERROR.CONNECT_FAILURE);
            const candidate = body.subarray(0x7c);
            const network = peer.scan(room => sameSession(room, candidate))[0];
            if (!network) return failure(ERROR.NOT_FOUND);
            if (network[OFFSET.ACCEPT_POLICY] === 1) return failure(ERROR.REJECTED);
            if (network[OFFSET.NODE_COUNT] >= network[OFFSET.MAX_NODES]) return failure(ERROR.FULL);
            const version = body.readUInt32LE(0x74);
            const expected = node(network, 0).readUInt16LE(OFFSET.NODE_VERSION);
            if (version !== expected) return failure(version < expected ? ERROR.VERSION_LOW : ERROR.VERSION_HIGH);
            const ownNode = nodeInfo({ ip: peer.ip, mac, name: body.subarray(0x44, 0x65), version });
            peer.setName(ownNode.subarray(OFFSET.NODE_NAME, OFFSET.NODE_NAME + 32).toString('utf8').split('\0')[0]);
            info = network;
            state = 'joining';
            if (!peer.join(network, ownNode)) {
                leave();
                return failure(ERROR.CONNECT_FAILURE);
            }
            if (state === 'joining') {
                timer = setTimeout(() => {
                    leave();
                    failure(ERROR.TIMEOUT);
                }, joinTimeoutMs);
                timer.unref();
            }
        }
        function setHostInfo(type, body) {
            if (state !== 'host') return;
            const next = Buffer.from(info);
            if (type === TYPE.ADVERTISE) setAdvertise(next, body);
            else if (body[0] <= 3) next[OFFSET.ACCEPT_POLICY] = body[0];
            else return;
            if (peer.sync(next)) {
                info = next;
                send(TYPE.SYNC, info);
            }
        }
        function sendData(body) {
            if (state !== 'host' && state !== 'guest') return;
            const route = proxyInfo(body);
            const length = body.readUInt32LE(SIZE.PROXY_INFO);
            if (length !== body.length - SIZE.PROXY_DATA || length > MAX_DATAGRAM ||
                (route.src !== 0 && route.src !== peer.ip)) return;
            if ([0xffffffff, MASKED_BROADCAST].includes(route.dst)) route.dst = BCAST;
            const payload = body.subarray(SIZE.PROXY_DATA);
            if (route.protocol === 17) peer.datagram({ ...route, payload });
            else if (route.protocol === 6) streamProxy.fromClient(TYPE.PROXY_DATA, body);
        }
        const reader = streamDecoder((type, body) => {
            const now = Date.now();
            if (now - windowStart >= WINDOW_MS) {
                windowStart = now;
                frames = bytes = scans = 0;
            }
            if (++frames > MAX_FRAMES_PER_WINDOW || (bytes += body.length + SIZE.HEADER) > MAX_BYTES_PER_WINDOW) {
                socket.destroy();
                return false;
            }
            if (type === TYPE.INITIALIZE && body.length === 22) {
                if (!id) initialize(body);
            } else if (type === TYPE.PING && body.length === 2) {
                if (body[0] !== 0) send(TYPE.PING, body);
            } else if (type === TYPE.PASSPHRASE && body.length === 128) {
                const enable = body[0] === 0;
                if (publicNetwork !== enable) leave();
                publicNetwork = enable;
            } else if (!peer) {
                socket.destroy();
                return false;
            } else if (type === TYPE.SCAN && body.length === SIZE.SCAN) {
                scan(body);
            } else if (type === TYPE.CREATE && body.length >= SIZE.CREATE && body.length <= SIZE.CREATE + SIZE.ADVERTISE) {
                hostGame(body);
            } else if (type === TYPE.CONNECT && body.length === SIZE.CONNECT) {
                join(body);
            } else if (type === TYPE.CREATE_PRIVATE || type === TYPE.CONNECT_PRIVATE) {
                failure(ERROR.REJECTED);
            } else if (type === TYPE.DISCONNECT && body.length === 4) {
                leave();
            } else if ((type === TYPE.ACCEPT_POLICY && body.length === 1) ||
                       (type === TYPE.ADVERTISE && body.length <= SIZE.ADVERTISE)) {
                setHostInfo(type, body);
            } else if (type === TYPE.REJECT && body.length === 8) {
                const index = body.readUInt32LE(0);
                const row = state === 'host' && index > 0 && index < MAX_NODES ? node(info, index) : null;
                if (row?.[OFFSET.NODE_ONLINE] && peer.kick(row.readUInt32LE(0))) send(TYPE.REJECT_REPLY);
                else failure(ERROR.REJECT_FAILED);
            } else if (type === TYPE.PROXY_DATA && body.length >= SIZE.PROXY_DATA) {
                sendData(body);
            } else if (type === TYPE.EXTERNAL_PROXY_STATE) {
                // The client may have started a local P2P listener before learning the room uses the master proxy.
            } else if (type === TYPE.PROXY_CONNECT || type === TYPE.PROXY_CONNECT_REPLY || type === TYPE.PROXY_DISCONNECT) {
                streamProxy.fromClient(type, body);
            }
        });
        socket.on('data', chunk => {
            try { reader(chunk); } catch { socket.destroy(); }
        });
        socket.on('close', () => {
            if (peer && id && mac) rememberIdentity(id, mac);
            leave(true);
            clients.delete(socket);
            const now = connectionsByIp.get(address);
            if (now === 1) connectionsByIp.delete(address);
            else connectionsByIp.set(address, now - 1);
        });
        // Ryujinx responds to a server requester value of zero with the same two bytes.
        const keepalive = setInterval(() => send(TYPE.PING, Buffer.from([0, 1])), 30000);
        keepalive.unref();
        socket.once('close', () => clearInterval(keepalive));
    });
    server.on('error', error => log(`RyuLDN listener: ${error.message}`));
    return {
        listen: () => new Promise((resolve, reject) => {
            server.once('error', reject);
            server.listen(port, host, () => {
                server.off('error', reject);
                resolve(server.address());
            });
        }),
        close: () => new Promise(resolve => {
            for (const socket of clients) socket.destroy();
            server.close(resolve);
        })
    };
}

module.exports = { create };
