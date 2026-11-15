'use strict';
// Panel de actividad del servidor de salas. Solo escucha en 127.0.0.1: se ve desde el PC del administrador por un
// túnel SSH (scripts/panel.ps1). Todo es agregado: ni IPs ni nada personal, solo apodos y códigos de sala.
//
// Guarda en el directorio de estado (stats.json) la historia: cada minuto de las últimas 24 h y cada día de los
// últimos 30, más los contadores y la actividad reciente.

const fs = require('fs');
const path = require('path');
const http = require('http');
const crypto = require('crypto');

const MINUTES = 24 * 60, DAYS = 30, EVENTS = 100;
const SEARCH_WINDOW_MS = 5 * 60 * 1000;  // "consolas buscando": las que buscaron grupos en los últimos 5 minutos

function validCount(value) {
    return Number.isSafeInteger(value) && value >= 0;
}

function validRecord(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function counters(value) {
    if (!validRecord(value) || Object.entries(value).some(([key, count]) =>
        !/^[a-zA-Z0-9_.:-]{1,96}$/.test(key) || !validCount(count)))
        throw new Error('Invalid statistics counters');
    return Object.assign(Object.create(null), value);
}

function validateState(value) {
    if (!validRecord(value)) throw new Error('Invalid statistics state');
    const state = { counters: counters(value.counters ?? {}), minutes: [], days: [], events: [] };
    // Explicit null is malformed, while omitted fields support older states.
    if ('counters' in value) state.counters = counters(value.counters);
    for (const [key, limit] of [['minutes', MINUTES], ['days', DAYS], ['events', EVENTS]]) {
        if (!(key in value)) continue;
        const rows = value[key];
        if (!Array.isArray(rows) || rows.length > limit || rows.some(row => !validRecord(row)))
            throw new Error('Invalid statistics history');
        state[key] = rows.map(row => {
            if (key === 'events') {
                if (!validCount(row.t) || typeof row.text !== 'string' || row.text.length > 4096)
                    throw new Error('Invalid statistics event');
                return { t: row.t, text: row.text };
            }
            const fields = key === 'minutes' ? ['t', 'online', 'rooms', 'groups', 'bytes'] :
                ['peakOnline', 'bytes', 'searchers', 'appUsers'];
            if (fields.some(field => !validCount(row[field]))) throw new Error('Invalid statistics record');
            const clean = Object.fromEntries(fields.map(field => [field, row[field]]));
            if (key === 'days') {
                if (typeof row.date !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(row.date))
                    throw new Error('Invalid statistics day');
                clean.date = row.date;
                clean.counters = counters(row.counters);
            }
            return clean;
        });
    }
    return state;
}

function create({ stateDir, port, snapshot, traffic, capBytes, paused }) {
    const file = path.join(stateDir, 'stats.json');
    let state = validateState({}), saveAllowed = true;
    let descriptor;
    try {
        descriptor = fs.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
        const stat = fs.fstatSync(descriptor);
        if (!stat.isFile() || stat.size > 4 * 1024 * 1024) throw new Error('Statistics file is invalid or too large');
        const bytes = Buffer.alloc(stat.size + 1);
        const read = fs.readSync(descriptor, bytes, 0, bytes.length, 0);
        if (read !== stat.size) throw new Error('Statistics changed during load');
        state = validateState(JSON.parse(bytes.subarray(0, read).toString('utf8')));
    } catch (error) {
        if (error.code !== 'ENOENT') {
            // Preserve malformed state for inspection before starting fresh counters.
            try { fs.renameSync(file, file + '.invalid-' + crypto.randomBytes(8).toString('hex')); }
            catch { saveAllowed = false; }
        }
    } finally {
        if (descriptor !== undefined) fs.closeSync(descriptor);
    }

    // Huella de IP (sal aleatoria, solo en memoria) → último momento en que buscó: desde un juego o con la app abierta.
    const recent = { game: new Map(), app: new Map() };
    const salt = crypto.randomBytes(16);
    let todaySeen = { game: new Set(), app: new Set() };
    let lastBytes = traffic.bytes;
    const started = Date.now();

    const dayKey = () => new Date().toISOString().slice(0, 10);
    function today() {
        const key = dayKey();
        let d = state.days[state.days.length - 1];
        if (!d || d.date !== key) {
            d = { date: key, peakOnline: 0, bytes: 0, searchers: 0, appUsers: 0, counters: Object.create(null) };
            state.days.push(d);
            if (state.days.length > DAYS) state.days.splice(0, state.days.length - DAYS);
            todaySeen = { game: new Set(), app: new Set() };
        }
        return d;
    }

    function count(name, n = 1) {
        if (!/^[a-zA-Z0-9_.:-]{1,96}$/.test(name) || !validCount(n)) return;
        const add = target => { target[name] = Math.min(Number.MAX_SAFE_INTEGER, (target[name] || 0) + n); };
        add(state.counters);
        add(today().counters);
    }

    function event(text) {
        state.events.push({ t: Date.now(), text });
        if (state.events.length > EVENTS) state.events.splice(0, state.events.length - EVENTS);
    }

    // kind: 'game' (un juego buscando grupo) o 'app' (la lista de la app, que se refresca sola).
    function noteSearch(ip, kind = 'game') {
        const h = crypto.createHash('sha256').update(salt).update(ip).digest('hex').slice(0, 16);
        const d = today();
        recent[kind].set(h, Date.now());
        todaySeen[kind].add(h);
        d.searchers = todaySeen.game.size;
        d.appUsers = todaySeen.app.size;
    }

    function active(kind) {
        const limit = Date.now() - SEARCH_WINDOW_MS;
        for (const [h, t] of recent[kind]) if (t < limit) recent[kind].delete(h);
        return recent[kind].size;
    }

    function tick() {
        const s = snapshot();
        let bytes = traffic.bytes - lastBytes;
        if (bytes < 0) bytes = traffic.bytes;  // empezó un mes nuevo y el contador volvió a cero
        lastBytes = traffic.bytes;
        state.minutes.push({ t: Date.now(), online: s.online, rooms: s.rooms, groups: s.groups, bytes });
        if (state.minutes.length > MINUTES) state.minutes.splice(0, state.minutes.length - MINUTES);
        const d = today();
        d.peakOnline = Math.max(d.peakOnline, s.online);
        d.bytes += bytes;
        save();
    }

    function save() {
        if (!saveAllowed) return;
        try {
            fs.writeFileSync(file, JSON.stringify(state));
        } catch { /* el panel no debe tumbar el servidor */ }
    }

    function gameNames() {
        try {
            return JSON.parse(fs.readFileSync(path.join(stateDir, 'games.json'), 'utf8'));
        } catch {
            return {};
        }
    }

    function report() {
        today();
        return {
            now: { ...snapshot(), searching: active('game'), appOpen: active('app'), paused: paused(), uptimeMs: Date.now() - started },
            traffic: { month: traffic.month, bytes: traffic.bytes, cap: capBytes },
            counters: state.counters,
            today: state.days[state.days.length - 1],
            minutes: state.minutes,
            days: state.days,
            events: state.events.slice().reverse(),
            gameNames: gameNames(),
        };
    }

    setInterval(tick, 60000).unref();

    if (port) {
        const page = path.join(__dirname, 'dashboard.html');
        const server = http.createServer({ headersTimeout: 2500, requestTimeout: 3500,
            connectionsCheckingInterval: 500, keepAliveTimeout: 1000, maxHeaderSize: 2048 }, (req, res) => {
            const host = req.headersDistinct.host;
            if (req.headers.origin !== undefined || host?.length !== 1 ||
                ![`127.0.0.1:${req.socket.localPort}`, `localhost:${req.socket.localPort}`].includes(host[0])) {
                res.writeHead(403, { Connection: 'close' });
                return res.end('Local dashboard only');
            }
            if (req.method !== 'GET' || req.headers['transfer-encoding'] !== undefined ||
                ![undefined, '0'].includes(req.headers['content-length'])) {
                res.writeHead(405, { Allow: 'GET', Connection: 'close' });
                return res.end();
            }
            if (req.url === '/api/stats') {
                res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
                return res.end(JSON.stringify(report()));
            }
            if (req.url === '/' || req.url === '/index.html') {
                fs.readFile(page, (err, html) => {
                    res.writeHead(err ? 500 : 200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
                    res.end(err ? 'Falta dashboard.html junto a server.js' : html);
                });
                return;
            }
            res.writeHead(404);
            res.end();
        });
        server.maxConnections = 16;
        server.maxRequestsPerSocket = 20;
        server.setTimeout(4500, socket => socket.destroy());
        server.on('checkContinue', (_req, res) => { res.writeHead(417, { Connection: 'close' }); res.end(); });
        server.on('error', (e) => console.log(new Date().toISOString(), `panel: ${e.message}`));
        server.listen(port, '127.0.0.1');
    }

    return { count, event, noteSearch, save };
}

module.exports = { create, validateState };
