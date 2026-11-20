'use strict';

const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const base = require('./node-catalog');

// Slots are persistent identities. Deleted slots stay reserved in the ledger so
// saved rooms and older clients can never silently point at a different host.
const FIRST = 13, LAST = 23, MAX_CONFIG_BYTES = 16384;
const CATALOG_REQUEST = 0x38, CATALOG_RESPONSE = 0xb8;
const LOBBY_REQUEST = 0x39, LOBBY_RESPONSE = 0xb9;
const CAPABILITY = 0x40000, REGION_CAPABILITY = 0x80000, ROW_BYTES = 192, REGION_BYTES = 32, REGION_VERSION = 2;
const geographicGroups = ['unknown', 'asia', 'europe', 'americas', 'oceania', 'africa'];

function parseRegion(value) {
    if (typeof value !== 'string') throw new Error('Invalid node region');
    const label = value.trim();
    if (Buffer.byteLength(label, 'utf8') >= REGION_BYTES || /[\x00-\x1f\x7f]/.test(value) ||
        Buffer.from(label, 'utf8').toString('utf8') !== label)
        throw new Error('Node region must be 31 UTF-8 bytes or fewer');
    if (/^[0-5]$/.test(label)) return { region: Number(label), regionLabel: '' };
    const known = geographicGroups.indexOf(label.toLowerCase());
    if (known >= 0) return { region: known, regionLabel: '' };
    if (['china', 'cn', '中国', '中國'].includes(label.toLowerCase())) return { region: 1, regionLabel: 'China' };
    return { region: 0, regionLabel: label };
}

function publicIPv4(address) {
    if (!net.isIPv4(address)) return false;
    const [a, b, c] = address.split('.').map(Number);
    return a !== 0 && a !== 10 && a !== 127 && a < 224 &&
        !(a === 169 && b === 254) && !(a === 172 && b >= 16 && b <= 31) &&
        !(a === 192 && (b === 168 || (b === 0 && (c === 0 || c === 2)) || (b === 88 && c === 99))) &&
        !(a === 198 && b === 51 && c === 100) && !(a === 203 && b === 0 && c === 113) &&
        !(a === 100 && b >= 64 && b <= 127) && !(a === 198 && (b === 18 || b === 19));
}

function hostName(value) {
    if (typeof value !== 'string' || value.length < 1 || value.length > 127 || value !== value.toLowerCase()) return false;
    if (net.isIPv4(value)) return publicIPv4(value);
    if (/^[0-9.]+$/.test(value)) return false;
    return value.split('.').length >= 2 && value.split('.').every(label =>
        label.length >= 1 && label.length <= 63 && /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(label));
}

function validate(doc) {
    if (!doc || doc.version !== 1 || !Array.isArray(doc.nodes) || doc.nodes.length > LAST - FIRST + 1)
        throw new Error('Invalid custom node registry');
    const ids = new Set(), endpoints = new Set(base.nodes.map(n => `${n.host.toLowerCase()}:${n.port}`));
    return doc.nodes.map(node => {
        if (!node || !Number.isInteger(node.id) || node.id < FIRST || node.id > LAST || ids.has(node.id) ||
            !hostName(node.host) || !Number.isInteger(node.port) || node.port < 1 || node.port > 65535 ||
            !Number.isInteger(node.region) || node.region < 0 || node.region > 5 ||
            typeof node.name !== 'string' || !node.name.trim() || Buffer.byteLength(node.name, 'utf8') > 47 ||
            /[\x00-\x1f\x7f]/.test(node.name) || typeof node.enabled !== 'boolean')
            throw new Error('Invalid custom node entry');
        const endpoint = `${node.host}:${node.port}`;
        if (endpoints.has(endpoint)) throw new Error('Duplicate custom node endpoint');
        ids.add(node.id); endpoints.add(endpoint);
        const location = parseRegion(node.regionLabel ?? '');
        return Object.freeze({ id: node.id, provider: `custom-${node.id}`, protocol: 2,
            host: node.host, port: node.port, region: location.regionLabel === 'China' ? 1 : node.region, country: '',
            regionLabel: location.regionLabel,
            name_en: node.name, name_zh: node.name, enabled: node.enabled });
    }).sort((a, b) => a.id - b.id);
}

function readRegular(filename, max) {
    const info = fs.lstatSync(filename);
    if (!info.isFile() || info.isSymbolicLink() || info.size > max) throw new Error('Invalid custom node registry file');
    return fs.readFileSync(filename, 'utf8');
}

function load(filename, stateDir) {
    if (!filename) return [];
    if (!path.isAbsolute(filename)) throw new Error('Custom node registry path must be absolute');
    const entries = validate(JSON.parse(readRegular(filename, MAX_CONFIG_BYTES)));
    const ledgerPath = path.join(stateDir, 'custom-node-identities.json');
    let ledger = {};
    try { ledger = JSON.parse(readRegular(ledgerPath, MAX_CONFIG_BYTES)); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (!ledger || typeof ledger !== 'object' || Array.isArray(ledger) ||
        Object.entries(ledger).some(([id, endpoint]) => !/^(1[3-9]|2[0-3])$/.test(id) || typeof endpoint !== 'string'))
        throw new Error('Invalid custom node identity ledger');
    for (const node of entries) {
        const endpoint = `${node.host}:${node.port}`;
        if (ledger[node.id] && ledger[node.id] !== endpoint) throw new Error(`Custom node slot ${node.id} cannot be reused`);
        if (Object.entries(ledger).some(([id, saved]) => Number(id) !== node.id && saved === endpoint))
            throw new Error('A registered endpoint cannot move to another slot');
        ledger[node.id] = endpoint;
    }
    if (entries.length) {
        const temp = ledgerPath + `.tmp-${process.pid}`;
        const fd = fs.openSync(temp, 'wx', 0o600);
        try { fs.writeFileSync(fd, JSON.stringify(ledger) + '\n'); fs.fsyncSync(fd); }
        finally { fs.closeSync(fd); }
        try {
            fs.renameSync(temp, ledgerPath);
            const dir = fs.openSync(stateDir, 'r');
            try { fs.fsyncSync(dir); } finally { fs.closeSync(dir); }
        } finally { try { fs.unlinkSync(temp); } catch (e) { if (e.code !== 'ENOENT') throw e; } }
    }
    return entries;
}

function response(entries, sources, withRegions = false) {
    const header = Buffer.alloc(40); base.hash.copy(header); header[32] = entries.length;
    if (withRegions) header[33] = REGION_VERSION;
    const availableMask = entries.reduce((mask, node) => node.enabled &&
        publicIPv4(sources.get(node.provider)?.upstreamAddress) ? mask | (1 << node.id) : mask, 0);
    header.writeUInt32LE(availableMask >>> 0, 36);
    const rows = entries.map(node => {
        const row = Buffer.alloc(ROW_BYTES + (withRegions ? REGION_BYTES : 0));
        row.writeUInt32LE(node.id); row[4] = 2; row[5] = node.region;
        row.writeUInt16LE(node.port, 8); row.writeUInt16LE(node.enabled ? 1 : 0, 10);
        row.write(node.host, 12, 127, 'ascii'); row.write(node.name_en, 140, 47, 'utf8');
        const address = sources.get(node.provider)?.upstreamAddress;
        if (node.enabled && publicIPv4(address)) Buffer.from(address.split('.').map(Number)).copy(row, 188);
        if (withRegions) row.write(node.regionLabel, ROW_BYTES, REGION_BYTES - 1, 'utf8');
        return row;
    });
    return Buffer.concat([header, ...rows]);
}
module.exports = { FIRST, LAST, CAPABILITY, REGION_CAPABILITY, REGION_VERSION, CATALOG_REQUEST, CATALOG_RESPONSE,
    LOBBY_REQUEST, LOBBY_RESPONSE, validate, load, response, publicIPv4, parseRegion };
