// SPDX-License-Identifier: GPL-2.0-only
'use strict';
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { randomBytes } = require('node:crypto');
const { zstdCompressSync, zstdDecompressSync } = require('node:zlib');
const { reserveUdpPort, consolePeer, withStations, nodeInfo } = require('../test-crossplay');
const { waitFor } = require('../test-peer');

async function main() {
    const port = await reserveUdpPort();
    const yuzu = process.argv[3] === '--yuzu';
    const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'rc-eden-roommember-'));
    const relay = spawn(process.execPath, [path.resolve(__dirname, '../../relay/server.js'), '--port', '0'], {
        stdio: ['ignore', 'pipe', 'inherit'],
        env: { ...process.env, LANYWHERE_STATE_DIR: temp,
            LANYWHERE_CONTROL_BIND_ADDRESS: '127.0.0.1', LANYWHERE_UDP_BIND_ADDRESS: '127.0.0.1',
            LANYWHERE_OPS_PORT: '0', LANYWHERE_STATS_PORT: '0', LANYWHERE_PUBLIC_PORT: '0',
            LANYWHERE_LANPLAY_PORT: '0', ROOMCIRCUIT_RYULDN_PORT: '0', LANYWHERE_SHUTDOWN_GRACE_MS: '0',
            ROOMCIRCUIT_EMULATOR_BIND: '127.0.0.1', ROOMCIRCUIT_EDEN_PORT: yuzu ? '0' : String(port),
            ROOMCIRCUIT_YUZU_PORT: yuzu ? String(port) : '0',
        },
    });
    let log = '', host, client;
    relay.stdout.on('data', bytes => { log += bytes; });
    const stopped = once(relay, 'close');
    try {
        const [, nativePort] = await waitFor(() => log.match(/datos UDP en (\d+)/), 'relay start');
        for (const role of ['guest', 'host']) {
            host = await consolePeer(Number(nativePort), 'SwitchWire');
            if (role === 'guest') await host.host(33);
            const outbound = randomBytes(384), inbound = randomBytes(514);
            const tx = path.join(temp, 'out.zst'), rx = path.join(temp, 'in.zst');
            fs.writeFileSync(tx, zstdCompressSync(outbound));
            client = spawn(process.argv[2], [String(port), tx, rx, role], { stdio: 'inherit' });
            const childExit = once(client, 'close');
            let ip;
            if (role === 'guest') {
                const joined = await host.recv(0x84, bytes => bytes[1] === 3);
                ip = joined.readUInt32LE(2);
                const info = withStations(host.info, joined.subarray(2));
                host.send(7, info);
                host.send(4, Buffer.concat([Buffer.from([joined[0], 4]), info]));
            } else {
                await host.join(await host.search(77));
                host.send(4, Buffer.concat([Buffer.from([0, 3]), nodeInfo(host.ip, 'Switch')]));
                const sync = await host.recv(0x84, bytes => bytes[1] === 4);
                ip = sync.readUInt32LE(2 + 0x68);
            }
            const packet = await waitFor(() => host.data.find(b => b[4] === 0x82),
                'upstream game UDP at Switch socket');
            assert.equal(packet.readUInt32LE(5), ip);
            assert.equal(packet.readUInt16LE(9), 3456);
            assert.deepEqual(packet.subarray(13), outbound);
            const dst = Buffer.alloc(4);
            dst.writeUInt32BE(ip);
            host.datagram(dst, inbound);
            assert.equal((await childExit)[0], 0);
            assert.deepEqual(zstdDecompressSync(fs.readFileSync(rx)), inbound);
            host.send(3);
            host.close();
            host = client = undefined;
        }
    } finally {
        client?.kill();
        host?.close();
        relay.kill('SIGTERM');
        await stopped;
        fs.rmSync(temp, { recursive: true, force: true });
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
