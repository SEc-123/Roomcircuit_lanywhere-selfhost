'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const vm = require('node:vm');
const net = require('node:net');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { setTimeout: delay } = require('node:timers/promises');
const stats = require('./stats');
const { Tcp } = require('./lanplay');
const { Peer } = require('../../deploy/smoke-relay.cjs');

test('fresh nested state directory has working appointment storage on first server start', async t => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'new-relay-state-'));
    const state = path.join(directory, 'nested', 'state');
    const process = spawn(global.process.execPath, ['server/relay/server.js', '--port', '0'], {
        env: { ...global.process.env, LANYWHERE_STATE_DIR: state, LANYWHERE_STATS_PORT: '0',
            LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_OPS_PORT: '0', LANYWHERE_LANPLAY_PORT: '0',
            LANYWHERE_ENABLE_PUBLIC_UPSTREAM: '0', LANYWHERE_ENABLE_LANPLAY_UPSTREAM: '0',
            LANYWHERE_SHUTDOWN_GRACE_MS: '0' }, stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    process.stdout.on('data', x => { output += x; });
    process.stderr.on('data', x => { output += x; });
    let peer;
    t.after(async () => {
        peer?.close();
        if (process.exitCode === null) { const exit = once(process, 'exit'); process.kill(); await exit; }
        fs.rmSync(directory, { recursive: true, force: true });
    });
    for (let n = 0; n < 200 && !output.includes('datos UDP'); n++) await delay(20);
    assert.match(output, /control TCP en (\d+)/);
    peer = new Peer('storage', '127.0.0.1', Number(/control TCP en (\d+)/.exec(output)[1]));
    await peer.connect();
    const query = Buffer.alloc(56); query[0] = 1; query.fill(1, 8, 24);
    peer.send(0x26, query);
    assert.equal((await peer.frame(0xa6)).readUInt32LE(), 0);
    query[0] = 2;
    query.writeBigUInt64LE(BigInt(Math.floor(Date.now() / 1000) + 60), 32);
    query.writeBigUInt64LE(123n, 40);
    peer.send(0x26, query);
    assert.equal((await peer.frame(0xa6)).readUInt32LE(), 0);
    assert.equal(JSON.parse(fs.readFileSync(path.join(state, 'appointments.json'))).events.length, 1);
});

test('actual monthly state transition resumes update distribution after quota pause', () => {
    const source = fs.readFileSync(path.join(__dirname, 'server.js'), 'utf8');
    const start = source.indexOf('function checkMonth() {');
    const end = source.indexOf("const PAUSED_TEXT", start);
    let month = '2026-01', stops = 0, resumes = 0;
    const context = { monthNow: () => month, traffic: { month, bytes: 0 }, CAP_BYTES: 100,
        paused: false, appointmentPush: null, matches: { drain() {} }, streams: { shutdown() {} },
        updates: { pause() { stops++; }, resume() { resumes++; } }, rooms: new Map(), log() {} };
    vm.createContext(context);
    vm.runInContext(source.slice(start, end), context);
    vm.runInContext('traffic.bytes=100; checkMonth()', context);
    assert.equal(context.paused, true); assert.equal(stops, 1);
    month = '2026-02';
    vm.runInContext('checkMonth()', context);
    assert.equal(context.paused, false); assert.equal(resumes, 1);
    assert.equal(context.traffic.bytes, 0);
});

test('malformed valid JSON statistics are quarantined and cannot break later counters', t => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bad-statistics-'));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    const filename = path.join(dir, 'stats.json');
    const invalid = { counters: {}, days: null, events: [] };
    fs.writeFileSync(filename, JSON.stringify(invalid));
    const panel = stats.create({ stateDir: dir, port: 0, snapshot: () => ({ online: 0, rooms: 0, groups: 0 }),
        traffic: { month: '2026-01', bytes: 0 }, capBytes: 100, paused: () => false });
    assert.doesNotThrow(() => { panel.count('roomsCreated'); panel.event('test event'); panel.noteSearch('192.0.2.1'); panel.save(); });
    const saved = JSON.parse(fs.readFileSync(filename));
    assert.equal(saved.counters.roomsCreated, 1); assert.equal(saved.days[0].counters.roomsCreated, 1);
    const copies = fs.readdirSync(dir).filter(name => name.startsWith('stats.json.invalid-'));
    assert.equal(copies.length, 1);
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(dir, copies[0]))), invalid);
    for (const value of [null, [], { counters: null }, { minutes: [null] }, { days: [{ date: '2026-01-01' }] }, { events: [null] }])
        assert.throws(() => stats.validateState(value));
});

test('gateway LDN TCP retains a bounded amount even when a slow peer keeps ACKing', () => {
    const connection = new Tcp(0x0a0d0001, 1, 0x0a0d0002, 2, () => {});
    let closed = 0;
    connection.onClose = () => closed++;
    connection.state = 'established';
    assert.equal(connection.write(Buffer.alloc(32000)), true);
    const pending = connection.queuedBytes;
    connection.acked((connection.snd + 1) >>> 0);
    assert.equal(connection.queuedBytes, pending, 'forged future ACK does not free budget');
    connection.acked(connection.queue[0].end);
    assert(connection.queuedBytes < pending);
    assert.equal(connection.write(Buffer.alloc(40000)), false);
    assert.equal(connection.state, 'gone');
    assert.equal(connection.queue.length, 0);
    assert.equal(connection.queuedBytes, 0);
    assert.equal(closed, 1);
    assert.equal(connection.write(Buffer.alloc(1)), false);
});

test('an interrupted state restore cannot start a relay with mixed files', async t => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'pending-relay-restore-'));
    t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
    fs.writeFileSync(path.join(directory, '.restore-pending'), 'Switch Online restore in progress\n');
    const child = spawn(process.execPath, ['server/relay/server.js', '--port', '0'], {
        env: { ...process.env, LANYWHERE_STATE_DIR: directory, LANYWHERE_STATS_PORT: '0',
            LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_OPS_PORT: '0', LANYWHERE_LANPLAY_PORT: '0' },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', x => { output += x; });
    child.stderr.on('data', x => { output += x; });
    const [code] = await once(child, 'exit');
    assert.notEqual(code, 0);
    assert.match(output, /State restore is incomplete/);
    assert(!output.includes('control TCP en'));
});


test('healthcheck rejects an appointment store with corrupt persisted state', async t => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'unhealthy-appointments-'));
    fs.writeFileSync(path.join(directory, 'appointments.json'), '{bad');
    const reservation = net.createServer();
    await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
    const statsPort = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));
    const child = spawn(process.execPath, ['server/relay/server.js', '--port', '0'], {
        env: { ...process.env, LANYWHERE_STATE_DIR: directory, LANYWHERE_STATS_PORT: String(statsPort),
            LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_OPS_PORT: '0', LANYWHERE_LANPLAY_PORT: '0',
            LANYWHERE_ENABLE_PUBLIC_UPSTREAM: '0', LANYWHERE_ENABLE_LANPLAY_UPSTREAM: '0',
            LANYWHERE_SHUTDOWN_GRACE_MS: '0' }, stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', x => { output += x; });
    child.stderr.on('data', x => { output += x; });
    t.after(async () => {
        if (child.exitCode === null) { const exit = once(child, 'exit'); child.kill(); await exit; }
        fs.rmSync(directory, { recursive: true, force: true });
    });
    for (let n = 0; n < 200 && !output.includes('datos UDP'); n++) await delay(20);
    const match = /control TCP en (\d+)/.exec(output);
    assert(match, output);
    const health = spawn(process.execPath, ['deploy/healthcheck.cjs', '--once', '--live'], {
        env: { ...process.env, LANYWHERE_HEALTH_CONTROL_PORT: match[1], LANYWHERE_STATS_PORT: String(statsPort) },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let diagnostic = '';
    health.stdout.on('data', data => { diagnostic += data; });
    health.stderr.on('data', data => { diagnostic += data; });
    const [code] = await once(health, 'exit');
    assert.notEqual(code, 0);
    assert.match(diagnostic, /Appointment storage is unhealthy/);
    assert.equal(fs.readFileSync(path.join(directory, 'appointments.json'), 'utf8'), '{bad');
});
