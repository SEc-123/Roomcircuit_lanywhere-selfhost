'use strict';

const { STATUS } = require('./appointments');

const MAX_BODY_BYTES = 1024;
const BODY_TIMEOUT_MS = 5000;
const MAX_U64 = (1n << 64n) - 1n;
const LAST_PAGE = 0xffffffff;
const WIRE_PAGE_SIZE = 6;
const WIRE_REQUEST_BYTES = 56;
const WIRE_RESPONSE_BYTES = 264;
const OPERATIONS = Object.freeze({ list: 1, create: 2, ready: 3, leave: 4, rooms: 5, event: 6 });
const ACTION_FIELDS = Object.freeze({
    list: ['offset'], create: ['starts', 'game', 'app'], ready: ['id'], leave: ['id'], rooms: ['id'], event: ['id'],
});
const FAILURES = new Map([
    [STATUS.UNAVAILABLE, [503, 'unavailable']], [STATUS.INVALID, [400, 'invalid']],
    [STATUS.LIMIT, [429, 'limit']], [STATUS.NOT_FOUND, [404, 'not_found']],
    [STATUS.STORAGE, [503, 'storage']],
]);

function clock() {
    return Math.floor(Date.now() / 1000);
}

function parseId(value) {
    if (typeof value !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value)) return null;
    const id = BigInt(value);
    return id <= MAX_U64 ? id : null;
}

function parseHex(value) {
    if (typeof value !== 'string' || !/^[\da-f]{16}$/i.test(value)) return null;
    return BigInt(`0x${value}`);
}

function encodeRequest(input) {
    if (!input || typeof input !== 'object' || Array.isArray(input) ||
        typeof input.action !== 'string' || !Object.hasOwn(OPERATIONS, input.action) ||
        typeof input.identity !== 'string' || !/^[\da-f]{32}$/i.test(input.identity) || /^0+$/.test(input.identity)) return null;
    const fields = ACTION_FIELDS[input.action];
    if (Object.keys(input).some(key => key !== 'action' && key !== 'identity' && !fields.includes(key))) return null;

    const action = input.action;
    const data = Buffer.alloc(WIRE_REQUEST_BYTES);
    data[0] = OPERATIONS[action];
    Buffer.from(input.identity, 'hex').copy(data, 8);
    if (action === 'list') {
        const offset = input.offset === undefined ? 0 : input.offset;
        if (!Number.isInteger(offset) || offset < 0 || offset >= LAST_PAGE) return null;
        data.writeUInt32LE(offset, 4);
    } else if (action === 'create') {
        const game = parseHex(input.game);
        const app = input.app === undefined ? 0n : parseHex(input.app);
        if (!Number.isSafeInteger(input.starts) || input.starts <= 0 || !game || game === MAX_U64 || app === null) return null;
        data.writeBigUInt64LE(BigInt(input.starts), 32);
        data.writeBigUInt64LE(game, 40);
        data.writeBigUInt64LE(app, 48);
    } else {
        const id = parseId(input.id);
        if (id === null) return null;
        if (action === 'rooms' || action === 'event') return { action, id, identity: input.identity };
        data.writeBigUInt64LE(id, 24);
    }
    return { action, data, identity: input.identity };
}

function decodeResponse(data) {
    if (!Buffer.isBuffer(data) || data.length !== WIRE_RESPONSE_BYTES) throw new Error('Invalid appointment response');
    const now = Number(data.readBigUInt64LE(16));
    const status = data.readUInt32LE(0);
    if (status !== STATUS.OK) {
        const [code, error] = FAILURES.get(status) || FAILURES.get(STATUS.STORAGE);
        return { code, result: { now, error } };
    }
    const count = data.readUInt32LE(4);
    const next = data.readUInt32LE(8);
    if (count > WIRE_PAGE_SIZE) throw new Error('Invalid appointment row count');
    const rows = [];
    for (let i = 0; i < count; i++) {
        const p = 24 + i * 40;
        rows.push({
            id: data.readBigUInt64LE(p).toString(),
            starts: Number(data.readBigUInt64LE(p + 8)),
            game: data.readBigUInt64LE(p + 16).toString(16).padStart(16, '0'),
            app: data.readBigUInt64LE(p + 24).toString(16).padStart(16, '0'),
            interested: data.readUInt32LE(p + 32),
            ready: data[p + 36] === 1,
        });
    }
    return { code: 200, result: { now, rows, next: next === LAST_PAGE ? null : next } };
}

function write(res, status, value, close = false) {
    if (res.destroyed || res.writableEnded) return;
    const body = Buffer.from(JSON.stringify(value));
    res.writeHead(status, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': body.length,
        'Cache-Control': 'no-store',
        'X-Content-Type-Options': 'nosniff',
        ...(close ? { Connection: 'close' } : {}),
    });
    res.end(body);
}

function readBody(req) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let bytes = 0;
        const timeout = setTimeout(() => {
            finish(Object.assign(new Error('Appointment request body timeout'), { code: 'BODY_TIMEOUT' }));
            req.resume();
        }, BODY_TIMEOUT_MS);
        timeout.unref();

        function finish(error) {
            clearTimeout(timeout);
            req.off('data', receive);
            req.off('end', end);
            req.off('aborted', abort);
            if (error) return reject(error);
            req.off('error', finish);
            return resolve(Buffer.concat(chunks));
        }
        function end() { finish(); }
        function abort() { finish(new Error('Appointment request aborted')); }
        function receive(chunk) {
            bytes += chunk.length;
            if (bytes > MAX_BODY_BYTES) {
                finish(new RangeError('Appointment request too large'));
                req.resume();
                chunks.length = 0;
            } else {
                chunks.push(chunk);
            }
        }
        req.on('data', receive);
        req.once('end', end);
        req.once('aborted', abort);
        req.once('error', finish);
    });
}

function requestOrigin(req) {
    return `${req.socket.encrypted ? 'https' : 'http'}://${req.headers.host || ''}`;
}

function matchesOrigin(req, trustedOrigin) {
    if (typeof req.headers.origin !== 'string' || req.headers['sec-fetch-site'] === 'cross-site') return false;
    try {
        const allowed = new URL(trustedOrigin);
        return (allowed.protocol === 'http:' || allowed.protocol === 'https:') && req.headers.origin === allowed.origin;
    } catch {
        return false;
    }
}

function create({ store, available, rooms, rateAllowed, sourceAddress = req => req.socket.remoteAddress,
                  origin = requestOrigin, native = false }) {
    return async function handle(req, res) {
        const error = (status, code, close = false) => write(res, status, { now: clock(), error: code }, close);
        if (req.method !== 'POST') {
            res.setHeader('Allow', 'POST');
            return error(405, 'invalid', true);
        }
        const accepted = native
            ? req.headers['x-roomcircuit-client'] === 'native-v1' && req.headers.origin === undefined &&
                !Object.keys(req.headers).some(header => header.startsWith('sec-fetch-'))
            : matchesOrigin(req, typeof origin === 'function' ? origin(req) : origin);
        if (!accepted) return error(403, 'forbidden', true);
        if (!/^application\/json(?:\s*;\s*charset\s*=\s*utf-8)?$/i.test(req.headers['content-type'] || ''))
            return error(415, 'invalid', true);
        const length = req.headers['content-length'];
        if (length !== undefined && (!/^\d+$/.test(length) || Number(length) > MAX_BODY_BYTES))
            return error(413, 'invalid', true);
        if (!available()) return error(503, 'unavailable', true);

        let command;
        try {
            const body = await readBody(req);
            command = encodeRequest(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(body)));
        } catch (failure) {
            const status = failure.code === 'BODY_TIMEOUT' ? 408 : failure instanceof RangeError ? 413 : 400;
            return error(status, 'invalid', true);
        }
        if (!command) return error(400, 'invalid');
        if (!available()) return error(503, 'unavailable');
        try {
            const ip = sourceAddress(req);
            if (typeof ip !== 'string' || !ip || ip.length > 128) return error(400, 'invalid');
            if (!rateAllowed(ip, command.action)) return error(429, 'limit');

            if (command.action === 'rooms') {
                const now = clock();
                const list = await rooms(command.id, now);
                if (list === null) return write(res, 404, { now, error: 'not_found' });
                return write(res, 200, { now, rooms: list });
            }

            const connected = () => !req.aborted && !res.destroyed && !res.writableEnded;
            const data = command.action === 'event' ? await store.detail(command.id, command.identity) :
                await store.handle(command.data, true, connected, ip);
            if (!connected()) return;
            const { code, result } = decodeResponse(data);
            if (command.action === 'create' && code === 200) {
                const id = store.createdAppointment(command.data.readBigUInt64LE(32),
                    command.data.readBigUInt64LE(40), command.data.readBigUInt64LE(48));
                if (id != null) {
                    const detail = decodeResponse(await store.detail(BigInt(id), command.identity));
                    if (detail.code === 200 && detail.result.rows.length === 1)
                        result.created = detail.result.rows[0];
                }
            }
            return write(res, code, result);
        } catch {
            return error(503, 'storage');
        }
    };
}

module.exports = { create };
