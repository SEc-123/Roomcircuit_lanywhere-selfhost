'use strict';

const { createHash, timingSafeEqual } = require('node:crypto');
const { performance } = require('node:perf_hooks');
const roomMetadata = require('./room-metadata');
const roomGameVersion = require('./room-game-version');

const REQUEST = 0x36, RESPONSE = 0xb6, CAPABILITY = 131072;
const READ_BYTES = 52, WRITE_BYTES = 384, REPLY_BYTES = 32;
const WRITE = 1, READ = 2;
const NONE = 0, SHARED = 1, HIDDEN = 2;
const STATUS = { OK: 0, UNAVAILABLE: 1, INVALID: 2, ROOM_UNAVAILABLE: 3, NOT_HOST: 4, BUSY: 5 };
const LIFETIME_MS = 45_000;
const MAX_ENTRIES = 256, MAX_PER_SOURCE = 16;

function key(node, hostIp, identity) {
    return String(node) + ':' + String(hostIp) + ':' + identity.toString('hex');
}

function reply(status, mode = NONE, passcode = null) {
    const out = Buffer.alloc(REPLY_BYTES);
    out.writeUInt32LE(status);
    out[4] = mode;
    if (mode === SHARED && passcode) {
        out[5] = passcode.length;
        passcode.copy(out, 8);
    }
    return out;
}

function create({ now = () => performance.now() } = {}) {
    const entries = new Map();

    function forget(id, entry) {
        if (entry.passcode) entry.passcode.fill(0);
        entries.delete(id);
    }

    function get(node, hostIp, identity) {
        const id = key(node, hostIp, identity);
        const entry = entries.get(id);
        if (!entry || entry.expires > now()) return entry;
        forget(id, entry);
        return undefined;
    }

    function sweep(time) {
        for (const [id, entry] of entries) {
            if (entry.expires <= time) forget(id, entry);
        }
    }

    // roomIsLive checks the directory's current direct locator, not the client-provided
    // room code or game filter. Requests can stage title data before its scan completes.
    function exchange(body, source, hostMask, roomIsLive) {
        if (!Buffer.isBuffer(body) || ![READ_BYTES, WRITE_BYTES, WRITE_BYTES + roomGameVersion.SIZE].includes(body.length))
            return reply(STATUS.INVALID);
        const op = body.readUInt32LE(), node = body.readUInt32LE(4);
        if ((op !== READ || body.length !== READ_BYTES) && (op !== WRITE || body.length === READ_BYTES))
            return reply(STATUS.INVALID);
        if (node < 2 || node >= 32 || !(hostMask & (1 << node))) return reply(STATUS.UNAVAILABLE);
        const hostIp = body.readUInt32LE(8);
        const identity = body.subarray(12, 50), session = identity.subarray(16, 32);
        if ((hostIp >>> 16) !== 0x0a0d || !(hostIp & 0xffff) || (hostIp & 0xffff) === 0xffff)
            return reply(STATUS.INVALID);
        if (!identity.readBigUInt64LE() || !session.some(Boolean) || !identity.subarray(32).some(Boolean))
            return reply(STATUS.INVALID);
        const mode = body[50], length = body[51];
        if (op === READ) {
            if (mode || length) return reply(STATUS.INVALID);
            const entry = get(node, hostIp, identity);
            if (!entry || !roomIsLive(node, hostIp, identity)) return reply(STATUS.ROOM_UNAVAILABLE);
            return reply(STATUS.OK, entry.mode, entry.passcode);
        }

        if (mode > HIDDEN || length > 24 || (mode === SHARED ? !length : length) || body.readUInt32LE(108) ||
            body.subarray(52 + length, 76).some(Boolean)) return reply(STATUS.INVALID);
        const password = body.subarray(52, 52 + length);
        if (!password.every(byte => byte >= 32 && byte <= 126)) return reply(STATUS.INVALID);

        // The host submits the random preimage over the main relay's control
        // connection. Public LAN Play advertisements only carry the commitment.
        const proof = body.subarray(76, 108);
        const digest = createHash('sha256').update(proof).update(body.subarray(4, 12)).digest();
        if (!proof.some(Boolean) || !timingSafeEqual(session, digest.subarray(0, 16)))
            return reply(STATUS.NOT_HOST);

        const declaration = Buffer.alloc(roomMetadata.PAYLOAD_BYTES);
        body.copy(declaration, 0, 120, 128); // Program ID, application ID, two names.
        body.copy(declaration, 8, 112, 120);
        body.copy(declaration, 16, 128);
        const title = roomMetadata.parse(declaration);
        if (!title) return reply(STATUS.INVALID);
        const version = body.length === WRITE_BYTES ? '' : roomGameVersion.parse(body.subarray(WRITE_BYTES), true);
        if (version === null || (version && (!title.applicationId || title.applicationId !== title.programId)))
            return reply(STATUS.INVALID);

        const live = roomIsLive(node, hostIp, identity);
        if (!live && mode !== NONE) return reply(STATUS.ROOM_UNAVAILABLE);
        const time = now();
        sweep(time);
        const id = key(node, hostIp, identity);
        const previous = entries.get(id);
        if (!previous && (entries.size >= MAX_ENTRIES ||
            [...entries.values()].filter(entry => entry.source === source).length >= MAX_PER_SOURCE))
            return reply(STATUS.BUSY);

        if (previous?.passcode) previous.passcode.fill(0);
        entries.set(id, {
            title, mode, version, passcode: mode === SHARED ? Buffer.from(password) : null,
            source: previous?.source ?? source, expires: time + LIFETIME_MS,
        });
        return reply(live ? STATUS.OK : STATUS.ROOM_UNAVAILABLE);
    }

    // Materialize projection at read time so cached pagination cannot retain a
    // title or password flag after a lease expires or the identified room closes.
    function decorate(row, node, hostIp, identity) {
        const entry = get(node, hostIp, identity);
        if (!entry) return row;
        const out = Buffer.from(row);
        const { applicationId, programId, en, zh } = entry.title;
        if (en || zh || applicationId || programId) {
            out.writeBigUInt64LE(applicationId, 16);
            out.writeBigUInt64LE(programId, 24);
            out[34] = 1;
            out.fill(0, 80, 336);
            out.write(en, 80, 127, 'utf8');
            out.write(zh, 208, 127, 'utf8');
        }
        out[35] = entry.mode;
        return out;
    }

    function gameVersion(node, hostIp, identity) {
        return get(node, hostIp, identity)?.version || '';
    }

    return { exchange, decorate, gameVersion, shutdown: () => {
        for (const [id, entry] of entries) forget(id, entry);
    } };
}

module.exports = { create, REQUEST, RESPONSE, CAPABILITY, STATUS };
