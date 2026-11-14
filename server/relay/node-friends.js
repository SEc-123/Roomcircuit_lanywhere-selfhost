'use strict';

const crypto = require('node:crypto');
const catalog = require('./node-catalog');
const REQUEST = 0x34, RESPONSE = 0xb4, REQUEST_BYTES = 240, RESPONSE_BYTES = 292;
const TTL_MS = 600000, SAMPLE_MS = 60000, PRESENCE_MS = 90000;
const MAX_PEOPLE = 8, MAX_SESSIONS = 512;
const ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const STATUS = { OK: 0, EXPIRED: 1, INVALID: 2, FULL: 3, BUSY: 4 };

function create({ nodes = catalog.nodes, enabledMask = catalog.mask, now = Date.now, randomBytes = crypto.randomBytes } = {}) {
    const sessions = new Map();
    const created = new Map();
    const rates = new Map();

    function sweep() {
        const time = now();
        for (const [code, session] of sessions) {
            for (const [key, person] of session.people)
                if (time - person.lastSeen >= PRESENCE_MS) session.people.delete(key);
            if (time >= session.expires || !session.people.size) {
                sessions.delete(code);
                created.delete(session.creatorKey);
            }
        }
        for (const [address, rate] of rates)
            if (time - rate.at > 60000) rates.delete(address);
    }

    function rate(address, creating) {
        const time = now();
        let limit = rates.get(address);
        if (!limit || time - limit.at >= 60000) {
            if (!limit && rates.size >= 4096) return false;
            limit = { at: time, requests: 0, creates: 0 };
            rates.set(address, limit);
        }
        limit.requests++;
        if (creating) limit.creates++;
        return limit.requests <= 600 && limit.creates <= 12;
    }

    function parse(body) {
        if (!Buffer.isBuffer(body) || body.length !== REQUEST_BYTES) return null;
        const op = body.readUInt32LE();
        const rawCode = body.subarray(4, 12), token = body.subarray(12, 28);
        const count = body.readUInt32LE(28);
        if (op < 1 || op > 5 || !token.some(Boolean) || count > nodes.length ||
            (op !== 3 && count) || rawCode[6] || rawCode[7]) return null;
        const code = rawCode.toString('ascii', 0, 6);
        if (op === 1 ? rawCode.some(Boolean) : !/^[A-HJ-NP-Z2-9]{6}$/.test(code)) return null;
        const samples = new Map();
        for (let i = 0; i < 13; i++) {
            const row = body.subarray(32 + i * 16, 48 + i * 16);
            if (i >= count) {
                if (row.some(Boolean)) return null;
                continue;
            }
            const id = row.readUInt32LE(), node = nodes[id];
            const sent = row[8], received = row[9], kind = row[10], status = row[11];
            const age = row.readUInt16LE(12);
            if (!node || samples.has(id) || row.readUInt16LE(14) || sent > 5 || received > sent ||
                kind !== (node.protocol === 2 ? 2 : 1) || status > 2 || age > 60 ||
                (status === 1 && (!received || !sent)) || (status !== 1 && received)) return null;
            samples.set(id, {
                id, sent, received, kind, status,
                median: row.readUInt16LE(4), jitter: row.readUInt16LE(6), at: now() - age * 1000,
            });
        }
        return { op, code, token: Buffer.from(token), key: token.toString('hex'), samples };
    }

    function newCode() {
        for (let attempt = 0; attempt < 20; attempt++) {
            let code = '';
            for (const byte of randomBytes(6)) code += ALPHABET[byte % ALPHABET.length];
            if (!sessions.has(code)) return code;
        }
        return null;
    }

    function respond(status, session, request) {
        const out = Buffer.alloc(RESPONSE_BYTES);
        out.writeUInt32LE(status);
        if (!session || !request) return out;
        out.write(session.code, 4, 6, 'ascii');
        request.token.copy(out, 12);
        out.writeUInt16LE(Math.min(600, Math.max(0, Math.ceil((session.expires - now()) / 1000))), 28);
        out[30] = session.people.size;
        const people = [...session.people.values()];
        out[31] = people.filter(person => [...person.samples.values()].some(sample => now() - sample.at < SAMPLE_MS)).length;
        const eligible = [];
        for (const node of nodes) {
            const row = out.subarray(32 + node.id * 20, 52 + node.id * 20);
            row.writeUInt32LE(node.id);
            const samples = people.map(person => person.samples.get(node.id))
                .filter(sample => sample && now() - sample.at < SAMPLE_MS);
            const reachable = samples.filter(sample => sample.status === 1 && sample.sent === 5 && sample.received >= 4);
            const median = Math.max(0, ...samples.map(sample => sample.median));
            const jitter = Math.max(0, ...samples.map(sample => sample.jitter));
            row.writeUInt32LE(median, 4);
            row.writeUInt32LE(jitter, 8);
            row[12] = samples.length;
            row[13] = reachable.length;
            row[14] = samples.length ? Math.min(...samples.map(sample => sample.sent)) : 0;
            row[15] = samples.length ? Math.min(...samples.map(sample => sample.received)) : 0;
            row[16] = node.protocol === 2 ? 2 : 1;
            if ((enabledMask & (1 << node.id)) && people.length && reachable.length === people.length)
                eligible.push({ node, median, jitter, received: row[15], row });
        }
        eligible.sort((a, b) => b.received - a.received || a.median - b.median ||
            a.jitter - b.jitter || a.node.id - b.node.id);
        const hosts = new Set();
        let rank = 0;
        for (const candidate of eligible) {
            if (hosts.has(candidate.node.host)) continue;
            hosts.add(candidate.node.host);
            candidate.row[17] = ++rank;
            if (rank === 3) break;
        }
        return out;
    }

    function exchange(body, address) {
        sweep();
        const request = parse(body);
        if (!request) return respond(STATUS.INVALID);
        if (!rate(address, request.op === 1)) return respond(STATUS.BUSY);
        let session;
        if (request.op === 1) {
            const creatorKey = address + ':' + request.key;
            session = sessions.get(created.get(creatorKey));
            if (!session) {
                if (sessions.size >= MAX_SESSIONS) return respond(STATUS.BUSY);
                const code = newCode();
                if (!code) return respond(STATUS.BUSY);
                session = { code, creatorKey, expires: now() + TTL_MS, people: new Map() };
                sessions.set(code, session);
                created.set(creatorKey, code);
            }
        } else session = sessions.get(request.code);
        if (!session) return respond(STATUS.EXPIRED);
        let person = session.people.get(request.key);
        if (request.op === 1 || request.op === 2) {
            if (!person) {
                if (session.people.size >= MAX_PEOPLE) return respond(STATUS.FULL);
                person = { samples: new Map(), lastSeen: now() };
                session.people.set(request.key, person);
            }
        } else if (!person) return respond(STATUS.EXPIRED);
        person.lastSeen = now();
        if (request.op === 3) person.samples = request.samples;
        if (request.op === 5) session.people.delete(request.key);
        const response = respond(STATUS.OK, session, request);
        if (!session.people.size) {
            sessions.delete(session.code);
            created.delete(session.creatorKey);
        }
        return response;
    }
    const timer = setInterval(sweep, 10000);
    timer.unref();
    return {
        exchange, sweep,
        shutdown() { clearInterval(timer); sessions.clear(); created.clear(); rates.clear(); },
        snapshot: () => ({ sessions: sessions.size,
            participants: [...sessions.values()].reduce((sum, item) => sum + item.people.size, 0) }),
    };
}

module.exports = { create, REQUEST, RESPONSE, REQUEST_BYTES, RESPONSE_BYTES, STATUS };
