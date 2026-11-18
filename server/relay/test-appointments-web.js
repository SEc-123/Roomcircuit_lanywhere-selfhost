'use strict';

const assert = require('node:assert/strict');
const { request } = require('node:http');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ConsolePeer, networkInfo, u64, until } = require('./test-public-e2e');
const { ryuPeer } = require('../eden/test-ryuldn-crossplay');

const GAME = 0x0100000000abc000n;
const WEB_ID = 'a2'.repeat(16);
const CONSOLE_ID = Buffer.alloc(16, 0xb1);

async function freePort() {
    const server = net.createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const port = server.address().port;
    await new Promise(resolve => server.close(resolve));
    return port;
}

async function appointment(peer, op, fields = {}) {
    const body = Buffer.alloc(56);
    body[0] = op;
    CONSOLE_ID.copy(body, 8);
    body.writeBigUInt64LE(fields.id || 0n, 24);
    body.writeBigUInt64LE(fields.starts || 0n, 32);
    body.writeBigUInt64LE(fields.game || 0n, 40);
    peer.send(0x26, body);
    const reply = await peer.frame(0xa6);
    assert.equal(reply.readUInt32LE(0), 0);
    return Array.from({ length: reply.readUInt32LE(4) }, (_, i) => {
        const p = 24 + i * 40;
        return { id: reply.readBigUInt64LE(p).toString(), starts: Number(reply.readBigUInt64LE(p + 8)),
            game: reply.readBigUInt64LE(p + 16), interested: reply.readUInt32LE(p + 32), ready: Boolean(reply[p + 36]) };
    });
}

test('browser, emulator UI and Switch share events, readiness, room lookup and alerts', { timeout: 15000 }, async t => {
    const state = await fs.mkdtemp(path.join(os.tmpdir(), 'rc-appointment-web-'));
    const web = await freePort();
    let ryu = await freePort();
    while (ryu === web) ryu = await freePort();
    const peers = [];
    const child = spawn(process.execPath, [path.join(__dirname, 'server.js'), '--port', '0'], {
        env: { ...process.env, LANYWHERE_STATE_DIR: state, LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_STATS_PORT: '0',
            LANYWHERE_OPS_PORT: '0', LANYWHERE_SHUTDOWN_GRACE_MS: '0', LANYWHERE_LANPLAY_PORT: '0',
            ROOMCIRCUIT_RYULDN_PORT: String(ryu), ROOMCIRCUIT_EDEN_PORT: '0', ROOMCIRCUIT_YUZU_PORT: '0',
            ROOMCIRCUIT_WEB_PORT: String(web), ROOMCIRCUIT_WEB_BIND: '127.0.0.1',
            ROOMCIRCUIT_WEB_ORIGIN: '', ROOMCIRCUIT_WEB_RELAY_HOST: '', LANYWHERE_CONTROL_BIND_ADDRESS: '127.0.0.1' },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    const exited = once(child, 'exit');
    t.after(async () => {
        for (const peer of peers) await peer.close();
        child.kill(); await exited;
        await fs.rm(state, { recursive: true, force: true });
    });
    let output = '';
    for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { output += bytes; });
    const nativePort = await until(() => {
        if (child.exitCode !== null) throw new Error(output);
        if (!output.includes('/appointments/')) return null;
        return Number(/datos UDP en (\d+)/.exec(output)?.[1]) || null;
    }, 'relay and web ready');
    const http = `http://127.0.0.1:${web}`;
    async function post(action, fields = {}) {
        const reply = await fetch(http + '/api/appointments', {
            method: 'POST', headers: { Origin: http, 'Content-Type': 'application/json' },
            body: JSON.stringify({ action, identity: WEB_ID, ...fields }),
        });
        const body = await reply.json();
        assert.equal(reply.status, 200, JSON.stringify(body));
        return body;
    }
    async function native(action, fields = {}, headerOverrides = {}, expected = 200) {
        const body = Buffer.from(JSON.stringify({ action, identity: WEB_ID, ...fields }));
        return new Promise((resolve, reject) => {
            const req = request(http + '/api/appointments/native', { method: 'POST', headers: {
                'Content-Type': 'application/json', 'Content-Length': body.length,
                'X-RoomCircuit-Client': 'native-v1', ...headerOverrides,
            } }, async res => {
                try {
                    const chunks = [];
                    for await (const bytes of res) chunks.push(bytes);
                    const value = JSON.parse(Buffer.concat(chunks));
                    assert.equal(res.statusCode, expected, JSON.stringify(value));
                    resolve(value);
                } catch (error) { reject(error); }
            });
            req.once('error', reject);
            req.end(body);
        });
    }
    const page = await fetch(http + '/appointments/');
    assert.equal(page.status, 200);
    assert.match(page.headers.get('content-security-policy'), /script-src 'self'/);
    assert.match(await page.text(), /RoomCircuit/);
    for (const name of ['styles.css', 'app.js']) assert.equal((await fetch(http + '/appointments/' + name)).status, 200);
    const options = await (await fetch(http + '/api/appointments/options')).json();
    assert.equal(options.nativeApi, 1);
    assert.deepEqual(options.endpoints.map(({ id, port }) => [id, port]), [['switch', nativePort], ['ryuldn', ryu]]);
    await native('list', {}, { 'X-RoomCircuit-Client': 'unknown' }, 403);
    await native('list', {}, { Origin: http }, 403);
    await native('list', {}, { 'Sec-Fetch-Mode': 'cors' }, 403);

    const consolePeer = new ConsolePeer('SwitchReader', nativePort);
    peers.push(consolePeer);
    await consolePeer.open({ udp: false });
    const starts = Math.floor(Date.now() / 1000) + 360;
    const { created } = await native('create', { starts, game: GAME.toString(16).padStart(16, '0') });
    assert.equal((await appointment(consolePeer, 1))[0].id, created.id);
    assert.equal((await post('event', { id: created.id })).rows[0].starts, starts);
    const nativeEvent = (await appointment(consolePeer, 2, { starts: BigInt(starts + 60), game: GAME + 1n }))
        .find(row => row.game === GAME + 1n);
    assert.equal((await native('event', { id: nativeEvent.id })).rows[0].starts, starts + 60);
    await native('ready', { id: created.id });
    const nativeReady = (await appointment(consolePeer, 3, { id: BigInt(created.id) }))[0];
    assert.equal(nativeReady.interested, 2);
    assert.equal((await post('event', { id: created.id })).rows[0].interested, 2);

    const watcher = new ConsolePeer('SwitchWatcher', nativePort);
    peers.push(watcher);
    await watcher.open({ udp: false });
    const subscription = Buffer.alloc(208);
    subscription.writeUInt32LE(1); subscription.writeBigUInt64LE(BigInt(created.id), 8);
    watcher.send(0x29, subscription);
    assert.equal((await watcher.frame(0xa9)).readBigUInt64LE(8), 0n); // subscribe acknowledgement
    await consolePeer.create(GAME);
    await consolePeer.publish(Buffer.concat([networkInfo([consolePeer], 4, GAME), u64(BigInt(created.id))]));
    const nativeRoomPush = await watcher.frame(0xa9);
    assert.equal(nativeRoomPush.readBigUInt64LE(8), BigInt(created.id));
    const browserRooms = (await post('rooms', { id: created.id })).rooms;
    assert.deepEqual(browserRooms, [{ host: 'SwitchReader', players: 1, max: 4, appointmentBound: true,
        crossplay: true, titleConfirmed: false }]);
    assert.deepEqual((await post('rooms', { id: nativeEvent.id })).rooms, []);

    const emu = await ryuPeer(ryu, 'EmulatorHost', peers);
    await emu.host();
    const emulatorRoomPush = await watcher.frame(0xa9);
    assert.equal(emulatorRoomPush.readBigUInt64LE(8), BigInt(created.id));
    assert.equal((await post('rooms', { id: created.id })).rooms[1]?.host, 'EmulatorHost');
    assert.equal((await native('rooms', { id: created.id })).rooms[1]?.host, 'EmulatorHost');
    const stopped = await native('leave', { id: created.id });
    assert.equal(stopped.rows.find(row => row.id === created.id).interested, 1);
});
