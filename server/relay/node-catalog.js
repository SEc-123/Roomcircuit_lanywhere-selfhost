'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const location = fs.existsSync(path.join(__dirname, 'node-catalog.json')) ?
    path.join(__dirname, 'node-catalog.json') : path.join(__dirname, '../../common/node-catalog.json');
const bytes = fs.readFileSync(location);
const catalog = JSON.parse(bytes);
if (catalog.version !== 1 || !Array.isArray(catalog.nodes) || catalog.nodes.length !== 13 ||
    catalog.nodes.some((node, index) => node.id !== index || ![0,1,2].includes(node.protocol) ||
        typeof node.provider !== 'string' || typeof node.host !== 'string' || !node.host ||
        !Number.isInteger(node.port) || node.port < 1 || node.port > 65535 ||
        !Number.isInteger(node.region) || node.region < 0 || node.region > 5))
    throw new Error('Invalid node catalog');
const nodes = Object.freeze(catalog.nodes.map(node => Object.freeze(node)));
module.exports = { nodes, hash: crypto.createHash('sha256').update(bytes).digest(),
    mask: (1 << nodes.length) - 1, version: catalog.version };
