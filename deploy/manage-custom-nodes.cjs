#!/usr/bin/env node
'use strict';

// Manage the administrator's persistent allowlist. The running server reads it
// only on startup; this command does not restart services or change clients.
const fs = require('node:fs');
const path = require('node:path');
const registry = require(fs.existsSync(path.join(__dirname, 'custom-node-registry.js')) ?
    './custom-node-registry.js' : '../server/relay/custom-node-registry.js');
const filename = '/var/lib/switch-online/custom-nodes.json';
const ledger = '/var/lib/switch-online/custom-node-identities.json';
const usage = 'Usage: manage-custom-nodes.cjs add ID HOST PORT REGION NAME | disable ID | list';
function regular(file) {
    const stat = fs.lstatSync(file);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 16384) throw Error('Unsafe registry file');
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}
function main() {
    const [op, ...args] = process.argv.slice(2);
    let config;
    try { config = regular(filename); } catch (e) { if (e.code !== 'ENOENT') throw e; config = {version: 1, nodes: []}; }
    registry.validate(config);
    if (op === 'list' && args.length === 0) return console.log(JSON.stringify(config, null, 2));
    if (op === 'add' && args.length === 5) {
        const [id, host, port, region, name] = args;
        if (!/^\d+$/.test(id) || !/^\d+$/.test(port)) throw Error(usage);
        const node = {id: Number(id), host, port: Number(port), ...registry.parseRegion(region), name, enabled: true};
        const existing = config.nodes.find(value => value.id === node.id);
        if (existing && (existing.host !== host || existing.port !== node.port)) throw Error('Slot belongs to another endpoint');
        if (existing) Object.assign(existing, node); else config.nodes.push(node);
    } else if (op === 'disable' && args.length === 1 && /^\d+$/.test(args[0])) {
        const node = config.nodes.find(value => value.id === Number(args[0]));
        if (!node) throw Error('Unknown slot');
        node.enabled = false;
    } else throw Error(usage);
    registry.validate(config);
    let identities = {};
    try { identities = regular(ledger); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    for (const node of config.nodes) {
        const endpoint = `${node.host}:${node.port}`;
        if ((identities[node.id] && identities[node.id] !== endpoint) ||
            Object.entries(identities).some(([id, value]) => Number(id) !== node.id && value === endpoint))
            throw Error('Existing node identity cannot be reused');
    }
    const temp = `${filename}.tmp-${process.pid}`;
    const fd = fs.openSync(temp, 'wx', 0o644);
    try { fs.fchmodSync(fd, 0o644); fs.writeFileSync(fd, JSON.stringify(config, null, 2) + '\n'); fs.fsyncSync(fd); }
    finally { fs.closeSync(fd); }
    try {
        fs.renameSync(temp, filename);
        const dir = fs.openSync(path.dirname(filename), 'r');
        try { fs.fsyncSync(dir); } finally { fs.closeSync(dir); }
    } finally { try { fs.unlinkSync(temp); } catch (e) { if (e.code !== 'ENOENT') throw e; } }
    console.log('Saved. Restart the relay during an idle maintenance window to apply.');
}
try { main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
