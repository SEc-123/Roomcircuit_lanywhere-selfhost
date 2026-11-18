'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');
const test = require('node:test');
const appointments = require('./appointments');
const browser = require('./appointments-http');

const SWITCH_ID = '01000000000000000000000000000000';
const WEB_ID = '02000000000000000000000000000000';
const GAME = '0100000000004321';
const APP = '0100000000001234';

function nativeRequest(operation, { id = '0', starts = 0 } = {}) {
    const body = Buffer.alloc(56);
    body[0] = operation;
    Buffer.from(SWITCH_ID, 'hex').copy(body, 8);
    body.writeBigUInt64LE(BigInt(id), 24);
    body.writeBigUInt64LE(BigInt(starts), 32);
    if (operation === 2) {
        body.writeBigUInt64LE(BigInt(`0x${GAME}`), 40);
        body.writeBigUInt64LE(BigInt(`0x${APP}`), 48);
    }
    return body;
}

async function fixture(t, options = {}) {
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'roomcircuit-http-'));
    const store = appointments.create({ filename: path.join(dir, 'appointments.json'), maxPerSource: 2 });
    const server = http.createServer(browser.create({
        store, available: () => true, rateAllowed: () => true,
        rooms: (id, now) => store.activeAppointment(id, now) ? [{ name: 'Crossplay' }] : null,
        ...options,
    }));
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    t.after(async () => {
        const closed = once(server, 'close');
        server.close();
        await closed;
        await fs.rm(dir, { recursive: true, force: true });
    });
    const base = `http://127.0.0.1:${server.address().port}`;
    async function request(value, { origin = base, extraHeaders, method = 'POST', raw } = {}) {
        const headers = { 'Content-Type': 'application/json', ...(origin !== null ? { Origin: origin } : {}), ...extraHeaders };
        const body = method === 'POST' ? (raw ?? JSON.stringify({ identity: WEB_ID, ...value })) : undefined;
        const res = await fetch(`${base}/api/appointments`, { method, headers, body });
        assert.equal(res.headers.get('cache-control'), 'no-store');
        assert.equal(res.headers.get('access-control-allow-origin'), null);
        return { code: res.status, body: await res.json() };
    }
    return { request, store };
}

test('Switch wire and web share appointment creation, readiness, and source budget', async t => {
    const { request, store } = await fixture(t);
    const current = Math.floor(Date.now() / 1000);
    const nativeCreated = await store.handle(nativeRequest(2, { starts: current + 60 }), true, () => true, '127.0.0.1');
    assert.equal(nativeCreated.readUInt32LE(0), appointments.STATUS.OK);
    const id = nativeCreated.readBigUInt64LE(24).toString();

    const webListed = await request({ action: 'list' });
    assert.equal(webListed.code, 200);
    assert.deepEqual(webListed.body.rows, [{ id, starts: current + 60, game: GAME, app: APP, interested: 0, ready: false }]);
    assert.equal(webListed.body.next, null);
    assert.deepEqual((await request({ action: 'rooms', id })).body.rooms, [{ name: 'Crossplay' }]);

    const webReady = await request({ action: 'ready', id });
    assert.equal(webReady.body.rows[0].ready, true);
    const switchReady = await store.handle(nativeRequest(3, { id }), true, () => true, '127.0.0.1');
    assert.equal(switchReady.readUInt32LE(24 + 32), 2);
    assert.equal((await request({ action: 'leave', id })).body.rows[0].interested, 1);

    const webCreated = await request({ action: 'create', starts: current + 120, game: GAME, app: APP });
    assert.equal(webCreated.code, 200);
    assert.equal(webCreated.body.created.starts, current + 120);
    const switchListed = await store.handle(nativeRequest(1), true, () => true, '127.0.0.1');
    assert.equal(switchListed.readUInt32LE(4), 2);
    assert.equal(Number(switchListed.readBigUInt64LE(24 + 40 + 8)), current + 120);
    assert.equal((await request({ action: 'create', starts: current + 180, game: GAME })).code, 429);
});

test('create returns the precise appointment even when it lies outside the current page', async t => {
    const { request, store } = await fixture(t);
    const current = Math.floor(Date.now() / 1000);
    for (let i = 1; i <= 6; i++) {
        const native = await store.handle(nativeRequest(2, { starts: current + 60 * i }), true, () => true, `192.0.2.${i}`);
        assert.equal(native.readUInt32LE(0), appointments.STATUS.OK);
    }
    const created = await request({ action: 'create', starts: current + 6000, game: GAME, app: APP });
    assert.equal(created.code, 200);
    assert.equal(created.body.rows.length, 6);
    assert.equal(created.body.next, 6);
    assert.equal(created.body.created.starts, current + 6000);
    assert(!created.body.rows.some(row => row.id === created.body.created.id));
    const detail = await request({ action: 'event', id: created.body.created.id });
    assert.equal(detail.code, 200);
    assert.deepEqual(detail.body.rows, [created.body.created]);
    assert.equal((await request({ action: 'event', id: '7' })).code, 404);
});

test('HTTP adapter rejects unrelated origins and invalid requests before mutation', async t => {
    const { request } = await fixture(t);
    assert.equal((await request({ action: 'list' }, { origin: 'https://example.invalid' })).code, 403);
    assert.equal((await request({ action: 'list' }, { origin: null })).code, 403);
    assert.equal((await request({}, { method: 'GET' })).code, 405);
    assert.equal((await request({ action: 'list' }, { raw: 'x'.repeat(1025) })).code, 413);
    assert.equal((await request({ action: 'list', offset: 0xffffffff })).code, 400);
    assert.equal((await request({ action: 'ready', id: '18446744073709551616' })).code, 400);
    assert.equal((await request({ action: 'rooms', id: '7' })).code, 404);
    assert.equal((await request({ action: 'create', starts: 60, game: 'ffffffffffffffff' })).code, 400);
    assert.equal((await request({ action: 'list', unrelated: 0 })).code, 400);
    assert.deepEqual((await request({ action: 'list' })).body.rows, []);
});
