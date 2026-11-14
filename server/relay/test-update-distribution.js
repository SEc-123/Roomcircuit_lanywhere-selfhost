'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');
const { spawn } = require('node:child_process');
const { setTimeout: delay } = require('node:timers/promises');
const { test } = require('node:test');
const updates = require('./update-distribution');

async function temporary(t) {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'lanywhere-update-'));
    t.after(() => fs.rm(directory, { recursive: true, force: true }));
    return directory;
}

async function install(directory, bytes = crypto.randomBytes(70000)) {
    const hash = crypto.createHash('sha256').update(bytes).digest();
    const manifest = Buffer.alloc(512);
    manifest.write('LNU1');
    for (const [offset, value] of [[4, 1], [8, 0], [12, 7], [16, 1], [20, 1], [24, bytes.length], [28, bytes.length], [32, 22]])
        manifest.writeUInt32LE(value, offset);
    hash.copy(manifest, 36);
    manifest.write('An update', 68); manifest.write('更新', 228);
    const file = path.join(directory, hash.toString('hex') + '.lup');
    await fs.writeFile(file, bytes);
    await fs.writeFile(path.join(directory, 'next.manifest'), manifest);
    await fs.rename(path.join(directory, 'next.manifest'), path.join(directory, 'current.manifest'));
    return { manifest, bytes, hash, file };
}

function request(hash, offset) {
    const body = Buffer.alloc(36); hash.copy(body); body.writeUInt32LE(offset, 32); return body;
}

async function until(condition, description) {
    const deadline = Date.now() + 4000;
    while (Date.now() < deadline) {
        const result = condition(); if (result) return result;
        await delay(10);
    }
    throw new Error('Timed out: ' + description);
}

async function moduleServer(t, directory, options = {}) {
    const sockets = new Set();
    const frame = (socket, type, body) => {
        const head = Buffer.alloc(3); head.writeUInt16LE(body.length + 1); head[2] = type;
        socket.write(Buffer.concat([head, body])); return true;
    };
    const manager = updates.create({ directory, intervalMs: 0, send: frame,
        error: (socket, code) => frame(socket, 0x85, Buffer.from([code])), ...options });
    const server = net.createServer(socket => {
        sockets.add(socket); socket.on('error', () => {});
        socket.on('close', () => { manager.close(socket); sockets.delete(socket); });
        let pending = Buffer.alloc(0);
        socket.on('data', chunk => {
            pending = Buffer.concat([pending, chunk]);
            while (pending.length >= 3 && pending.length >= 2 + pending.readUInt16LE()) {
                const size = pending.readUInt16LE();
                manager.handle(socket, pending[2], pending.subarray(3, size + 2));
                pending = pending.subarray(size + 2);
            }
        });
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(async () => { manager.shutdown(); for (const s of sockets) s.destroy(); await new Promise(resolve => server.close(resolve)); });
    return { port: server.address().port, manager };
}

async function peer(t, port) {
    const socket = net.connect(port, '127.0.0.1'), frames = [];
    let pending = Buffer.alloc(0), failure;
    socket.on('error', error => { failure = error; });
    socket.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 3 && pending.length >= 2 + pending.readUInt16LE()) {
            const size = pending.readUInt16LE();
            frames.push({ type: pending[2], body: Buffer.from(pending.subarray(3, size + 2)) });
            pending = pending.subarray(size + 2);
        }
    });
    await once(socket, 'connect'); t.after(() => socket.destroy());
    return { socket,
        send(type, body = Buffer.alloc(0)) {
            const header = Buffer.alloc(3); header.writeUInt16LE(body.length + 1); header[2] = type;
            socket.write(Buffer.concat([header, body]));
        },
        frame: () => until(() => { if (failure) throw failure; return frames.shift(); }, 'protocol reply') };
}

test('update descriptor stays on immutable package across manifest publication and resume', async t => {
    const directory = await temporary(t), first = await install(directory);
    const server = await moduleServer(t, directory);
    const p = await peer(t, server.port);
    p.send(0x30); assert.deepEqual(await p.frame(), { type: 0xb0, body: first.manifest });
    const received = [];
    p.send(0x31, request(first.hash, 0));
    let frame = await p.frame(); assert.equal(frame.type, 0xb1); received.push(frame.body.subarray(4));
    const second = await install(directory, crypto.randomBytes(1100));
    for (let offset = updates.CHUNK_BYTES; offset < first.bytes.length; offset += updates.CHUNK_BYTES) {
        p.send(0x31, request(first.hash, offset)); frame = await p.frame();
        assert.equal(frame.type, 0xb1); assert.equal(frame.body.readUInt32LE(), offset); received.push(frame.body.subarray(4));
    }
    assert.deepEqual(Buffer.concat(received), first.bytes);
    p.send(0x31, request(second.hash, 0)); assert.equal((await p.frame()).body[0], 4);
    const fresh = await peer(t, server.port);
    fresh.send(0x31, request(first.hash, 1)); assert.equal((await fresh.frame()).body[0], 11);
    fresh.send(0x31, request(second.hash, 11)); frame = await fresh.frame();
    assert.equal(frame.type, 0xb1); assert.deepEqual(frame.body.subarray(4), second.bytes.subarray(11));
    p.socket.destroy(); fresh.socket.destroy();
    await until(() => server.manager.snapshot().transfers === 0, 'descriptor cleanup');
});

test('update transfer slots, request sizes, file safety and backpressure are bounded', async t => {
    const directory = await temporary(t), snapshot = await install(directory);
    const server = await moduleServer(t, directory, { maxTransfers: 1, maxPerIp: 1, intervalMs: 100 });
    const first = await peer(t, server.port), second = await peer(t, server.port);
    first.send(0x31, request(snapshot.hash, 0)); assert.equal((await first.frame()).type, 0xb1);
    second.send(0x31, request(snapshot.hash, 0)); assert.equal((await second.frame()).body[0], 7);
    second.send(0x30, Buffer.alloc(1)); assert.equal((await second.frame()).body[0], 4);
    first.send(0x31, request(snapshot.hash, snapshot.bytes.length)); assert.equal((await first.frame()).body[0], 4);
    first.send(0x31, request(snapshot.hash, 0)); first.send(0x31, request(snapshot.hash, 0));
    const answers = [await first.frame(), await first.frame()];
    assert.deepEqual(answers.map(x => x.type).sort(), [0x85, 0xb1]);
    first.socket.destroy(); await until(() => server.manager.snapshot().transfers === 0, 'release transfer slot');
    await fs.unlink(snapshot.file); await fs.symlink('/dev/null', snapshot.file);
    second.send(0x31, request(snapshot.hash, 0)); assert.equal((await second.frame()).body[0], 11);
    assert.equal(server.manager.snapshot().transfers, 0);
    await fs.writeFile(path.join(directory, 'current.manifest'), Buffer.alloc(513));
    second.send(0x30); assert.equal((await second.frame()).body[0], 11);
});

test('real relay updater socket cannot join rooms and active game connections cannot update', { timeout: 12000 }, async t => {
    const directory = await temporary(t), snapshot = await install(directory);
    const child = spawn(process.execPath, ['server/relay/server.js', '--port', '0'], {
        cwd: path.join(__dirname, '../..'), stdio: ['ignore', 'pipe', 'pipe'],
        env: { ...process.env, LANYWHERE_UPDATE_DIR: directory, LANYWHERE_STATE_DIR: path.join(directory, 'state'),
            LANYWHERE_STATS_PORT: '0', LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_OPS_PORT: '0', LANYWHERE_LANPLAY_PORT: '0',
            LANYWHERE_ENABLE_PUBLIC_UPSTREAM: '0', LANYWHERE_ENABLE_LANPLAY_UPSTREAM: '0', LANYWHERE_ENABLE_COMMUNITY_DIRECT: '0' } });
    let output = ''; child.stdout.on('data', data => output += data); child.stderr.on('data', data => output += data);
    t.after(async () => { child.kill(); await once(child, 'exit').catch(() => {}); });
    const port = await until(() => Number(output.match(/control TCP en (\d+)/)?.[1]), 'relay bind');
    const p = await peer(t, port);
    p.send(0x30); assert.deepEqual(await p.frame(), { type: 0xb0, body: snapshot.manifest });
    p.send(1, Buffer.alloc(40)); assert.equal((await p.frame()).body[0], 4);
    p.send(5); assert.equal((await p.frame()).type, 0x86);
    p.send(0x31, request(snapshot.hash, 0)); assert.equal((await p.frame()).type, 0xb1);
    const game = await peer(t, port);
    game.send(1, Buffer.alloc(40)); assert.equal((await game.frame()).type, 0x81);
    game.send(0x30); assert.equal((await game.frame()).body[0], 4);
    game.send(3); game.send(5); assert.equal((await game.frame()).type, 0x86);
});
