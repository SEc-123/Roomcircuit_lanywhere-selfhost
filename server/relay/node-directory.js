'use strict';

const { performance } = require('node:perf_hooks');
const catalog = require('./node-catalog');
const locator = require('./node-locator');
const CAPABILITY = 32768, CATALOG_REQUEST = 0x32, CATALOG_RESPONSE = 0xb2;
const REQUEST = 0x33, RESPONSE = 0xb3, REALM_REQUEST = 0x35, REALM_RESPONSE = 0xb5;
const PAGE_SIZE = 6, HEADER_BYTES = 16, ROW_BYTES = 408, END = 0xffffffff;
const DIRECT = 1, GATEWAY = 2;

function create({ source, providerIds, communityDirect, nodes = catalog.nodes, hash = catalog.hash,
    now = () => performance.now() }) {
    const enabled = new Set(['self', ...providerIds]);
    const allowedMask = (1 << nodes.length) - 1;
    const hostMask = nodes.reduce((mask, node) => mask | ((enabled.has(node.provider) &&
        (node.protocol !== 1 || communityDirect)) ? 1 << node.id : 0), 0);
    const snapshots = new Map();
    let generation = 0;

    function status() {
        const sources = source();
        const result = Buffer.alloc(40);
        hash.copy(result);
        result.writeUInt32LE(hostMask, 32);
        result.writeUInt32LE(nodes.reduce((mask, node) =>
            mask | (sources.has(node.provider) ? 1 << node.id : 0), 0), 36);
        return result;
    }

    function room(node, item) {
        const data = Buffer.alloc(ROW_BYTES), summary = item.summary;
        summary.copy(data);
        data.writeUInt32LE(node.id, 336);
        const game = summary.readBigUInt64LE(8), code = summary.toString('ascii', 0, 6);
        let direct = node.protocol === 0;
        if (node.protocol === 1) direct = communityDirect && game !== 0xffffffffffffffffn;
        if (node.protocol === 2) direct = locator.valid(item.locator, game, code, true);
        data.writeUInt32LE(direct ? DIRECT : GATEWAY, 340);
        if (direct && item.locator) item.locator.copy(data, 344);
        else data.write(code, 344 + 44, 6, 'ascii');
        return data;
    }

    function page(request) {
        if (!Buffer.isBuffer(request) || request.length !== 20) return null;
        const offset = request.readUInt32LE(), id = request.readUInt32LE(4), game = request.readBigUInt64LE(8);
        const mask = request.readUInt32LE(16);
        if (mask & ~allowedMask || (offset && !id)) return null;
        const time = now(), sources = source();
        for (const [key, value] of snapshots)
            if (time - value.at > 30000) snapshots.delete(key);
        let snapshot = id ? snapshots.get(id) : null;
        const selectedMask = mask;
        if (!id) {
            const rows = [], selected = nodes.filter(node => selectedMask & (1 << node.id));
            for (const node of selected) {
                for (const item of sources.get(node.provider) || []) {
                    if (!game || item.summary.readBigUInt64LE(8) === game) rows.push(room(node, item));
                }
            }
            rows.sort((a, b) => {
                const ga = a.readBigUInt64LE(8), gb = b.readBigUInt64LE(8);
                return (ga === gb ? 0 : ga < gb ? -1 : 1) || b[32] - a[32] ||
                    a.readUInt32LE(336) - b.readUInt32LE(336) ||
                    Buffer.compare(a.subarray(0, 6), b.subarray(0, 6));
            });
            const available = selected.filter(node => sources.has(node.provider)).length;
            snapshot = [...snapshots.values()].find(value => value.game === game && value.mask === selectedMask &&
                value.available === available && value.rows.length === rows.length &&
                value.rows.every((row, i) => row.equals(rows[i])));
            if (!snapshot) {
                do { generation = (generation + 1) >>> 0; } while (!generation || snapshots.has(generation));
                snapshot = { id: generation, at: time, rows, game, mask: selectedMask, available, total: selected.length };
                snapshots.set(generation, snapshot);
                if (snapshots.size > 16) snapshots.delete(snapshots.keys().next().value);
            }
        }
        const header = Buffer.alloc(HEADER_BYTES);
        header.writeUInt32LE(END, 4);
        if (!snapshot || snapshot.game !== game || snapshot.mask !== selectedMask || offset > snapshot.rows.length) {
            header[11] = 1;
            return header;
        }
        const rows = snapshot.rows.slice(offset, offset + PAGE_SIZE);
        header.writeUInt32LE(snapshot.id);
        header[8] = rows.length;
        header[9] = snapshot.available;
        header[10] = snapshot.total;
        if (offset + rows.length < snapshot.rows.length) header.writeUInt32LE(offset + rows.length, 4);
        return Buffer.concat([header, ...rows]);
    }
    return { status, page, enabledMask: hostMask, shutdown: () => snapshots.clear() };
}
module.exports = { create, CAPABILITY, CATALOG_REQUEST, CATALOG_RESPONSE, REQUEST, RESPONSE,
    REALM_REQUEST, REALM_RESPONSE, ROW_BYTES, HEADER_BYTES, DIRECT, GATEWAY };
