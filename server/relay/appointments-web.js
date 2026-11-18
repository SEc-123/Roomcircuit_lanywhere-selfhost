'use strict';

const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const api = require('./appointments-http');
const { games } = require('./web-appointment-games');

const ASSETS = {
    '/appointments/': ['index.html', 'text/html; charset=utf-8'],
    '/appointments/app.js': ['app.js', 'text/javascript; charset=utf-8'],
    '/appointments/styles.css': ['styles.css', 'text/css; charset=utf-8'],
};
const SECURITY = {
    'Cache-Control': 'no-store',
    'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
};
const OPTIONS_PATH = '/api/appointments/options';

function validOrigin(value) {
    try {
        const url = new URL(value);
        return ['http:', 'https:'].includes(url.protocol) && url.origin === value;
    } catch {
        return false;
    }
}

function validRelayHost(value) {
    if (!value || value.length > 253 || value === '0.0.0.0') return false;
    return net.isIPv4(value) || value.split('.').every(label =>
        /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/i.test(label));
}

function create({ port, bind, origin, relayHost, relayPorts, store, available, rooms, rateAllowed, log }) {
    if (!port) return;
    if (!net.isIPv4(bind) || (bind === '0.0.0.0' && !origin) ||
        (origin && !validOrigin(origin)) || (relayHost && !validRelayHost(relayHost)))
        throw new Error('Invalid appointments web bind, origin or relay host');

    const assets = new Map(Object.entries(ASSETS).map(([route, [file, type]]) =>
        [route, { bytes: fs.readFileSync(path.join(__dirname, 'web-appointments', file)), type }]));
    const permittedOrigins = new Set((origin ? [origin] : [
        `http://${bind}:${port}`,
        ...(bind === '127.0.0.1' ? [`http://localhost:${port}`] : []),
    ]).map(value => new URL(value).origin));

    function expectOrigin(req) {
        // Behind a TLS proxy, the configured URL and preserved Host are authoritative.
        const candidate = `${origin?.startsWith('https:') ? 'https' : 'http'}://${req.headers.host || ''}`;
        try {
            const value = new URL(candidate);
            const host = req.headers.host;
            return (host === value.host || (value.port === '' && host === value.hostname + (value.protocol === 'https:' ? ':443' : ':80'))) &&
                permittedOrigins.has(value.origin) ? value.origin : null;
        } catch {
            return null;
        }
    }

    const handle = api.create({ store, available, rooms, rateAllowed, origin: expectOrigin, sourceAddress(req) {
        const ip = req.socket.remoteAddress?.replace(/^::ffff:/, '');
        const forwarded = req.headers['x-real-ip'];
        // Only the configured same-machine HTTPS proxy may supply a client address.
        if (origin?.startsWith('https:') && ip === '127.0.0.1' && typeof forwarded === 'string' && net.isIP(forwarded))
            return forwarded.replace(/^::ffff:/, '');
        return ip;
    } });

    const server = http.createServer({ maxHeaderSize: 8192, headersTimeout: 5000,
        requestTimeout: 10000, keepAliveTimeout: 5000 }, (req, res) => {
        for (const [key, value] of Object.entries(SECURITY)) res.setHeader(key, value);
        const trusted = expectOrigin(req);
        if (!trusted) {
            res.writeHead(421, { Connection: 'close' });
            return res.end();
        }
        let uri;
        try { uri = new URL(req.url, trusted); }
        catch { res.writeHead(400); return res.end(); }
        if (uri.origin !== trusted) { res.writeHead(400); return res.end(); }
        if (uri.pathname === '/api/appointments') return void handle(req, res);
        if (req.method !== 'GET' && req.method !== 'HEAD') {
            res.writeHead(405, { Allow: 'GET, HEAD', Connection: 'close' });
            return res.end();
        }
        if (uri.pathname === '/' || uri.pathname === '/appointments') {
            res.writeHead(302, { Location: '/appointments/' + uri.search });
            return res.end();
        }
        if (uri.pathname === OPTIONS_PATH) {
            const host = relayHost || new URL(trusted).hostname;
            const options = {
                games, readyBefore: 1800, readyLease: 300,
                endpoints: relayPorts().filter(item => item.port).map(item => ({ ...item, host })),
            };
            const body = Buffer.from(JSON.stringify(options));
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': body.length });
            return res.end(req.method === 'HEAD' ? undefined : body);
        }
        const asset = assets.get(uri.pathname);
        if (!asset) { res.writeHead(404); return res.end(); }
        res.writeHead(200, { 'Content-Type': asset.type, 'Content-Length': asset.bytes.length });
        res.end(req.method === 'HEAD' ? undefined : asset.bytes);
    });
    server.maxConnections = 128;
    server.setTimeout(10000, socket => socket.destroy());
    const ready = new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(port, bind, () => {
            server.off('error', reject);
            server.on('error', error => log(`Appointments web: ${error.message}`));
            log(`Appointments http://${bind}:${port}/appointments/`);
            resolve();
        });
    });
    return { ready, close: () => { server.close(); server.closeAllConnections(); } };
}

module.exports = { create, validOrigin, validRelayHost };
