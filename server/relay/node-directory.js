'use strict';

const { performance } = require('node:perf_hooks');
const catalog = require('./node-catalog');
const locator = require('./node-locator');
const directRoomMetadata = require('./direct-room-metadata');
const customNodes = require('./custom-node-registry');
const roomGameVersion = require('./room-game-version');
const CAPABILITY = 32768, CATALOG_REQUEST = 0x32, CATALOG_RESPONSE = 0xb2;
const REQUEST = 0x33, RESPONSE = 0xb3, REALM_REQUEST = 0x35, REALM_RESPONSE = 0xb5;
const ANNOTATED_REQUEST = 0x37, ANNOTATED_RESPONSE = 0xb7;
const PAGE_SIZE = 6, HEADER_BYTES = 16, ROW_BYTES = 408, END = 0xffffffff;
const DIRECT = 1, GATEWAY = 2;

function create({ source, providerIds, communityDirect, nodes = catalog.nodes, hash = catalog.hash,
    now = () => performance.now(), metadata = directRoomMetadata.create({ now }) }) {
    const enabled = new Set(['self', ...providerIds]);
    const allowedMask = nodes.reduce((mask, node) => mask | (1 << node.id), 0) >>> 0;
    const hostMask = nodes.reduce((mask, node) => mask | ((enabled.has(node.provider) &&
        (node.protocol !== 1 || communityDirect)) ? 1 << node.id : 0), 0);
    const directMask = nodes.reduce((mask, node) => mask | (node.protocol === 2 ? (1 << node.id) : 0), 0) & hostMask;
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
        if (node.id >= customNodes.FIRST && !direct) return null;
        data.writeUInt32LE(direct ? DIRECT : GATEWAY, 340);
        if (direct && item.locator) {
            item.locator.copy(data, 344);
            if (node.id >= customNodes.FIRST) {
                if (!customNodes.publicIPv4(item.upstreamAddress)) return null;
                Buffer.from(item.upstreamAddress.split('.').map(Number)).copy(data, 344 + 52);
            }
        }
        else data.write(code, 344 + 44, 6, 'ascii');
        return data;
    }

    function isLive(sources, nodeId, hostIp, identity, expectedEndpoint = null) {
        if (!(directMask & (1 << nodeId))) return false;
        const provider = nodes.find(node => node.id === nodeId)?.provider;
        if (!provider) return false;
        let matches = 0;
        for (const { summary, locator: candidate, upstreamAddress } of sources.get(provider) || []) {
            if ((!expectedEndpoint || upstreamAddress === expectedEndpoint) && locator.valid(candidate, summary.readBigUInt64LE(8), summary.toString('ascii', 0, 6), true) &&
                candidate.readUInt32LE(40) === hostIp && candidate.subarray(0, 38).equals(identity)) {
                if (++matches > 1) return false;
            }
        }
        return matches === 1;
    }

    function exchangeMetadata(request, sourceAddress) {
        const sources = source();
        return metadata.exchange(request, sourceAddress, directMask,
            (id, hostIp, identity) => isLive(sources, id, hostIp, identity));
    }

    function page(request, annotated = false, withVersions = false) {
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
                    if (!game || item.summary.readBigUInt64LE(8) === game) {
                        const row = room(node, item);
                        if (row) rows.push(row);
                    }
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
        if (!annotated) return Buffer.concat([header, ...rows]);
        const annotatedRows = rows.map(row => {
            const node = row.readUInt32LE(336), hostIp = row.readUInt32LE(384);
            const identity = row.subarray(344, 382);
            const endpoint = node >= customNodes.FIRST ? [...row.subarray(396, 400)].join('.') : null;
            const direct = row.readUInt32LE(340) === DIRECT && isLive(sources, node, hostIp, identity, endpoint);
            const base = direct ? metadata.decorate(row, node, hostIp, identity) : row;
            if (!withVersions) return base;
            const version = Buffer.alloc(roomGameVersion.SIZE);
            const text = direct ? metadata.gameVersion(node, hostIp, identity) : node === 0 ?
                sources.get('self')?.find(item => item.summary.equals(row.subarray(0, 336)))?.gameVersion : null;
            if (text) version.write(text, 0, roomGameVersion.SIZE - 1, 'ascii');
            return Buffer.concat([base, version]);
        });
        return Buffer.concat([header, ...annotatedRows]);
    }
    return { status, page, exchangeMetadata, enabledMask: hostMask, shutdown: () => {
        snapshots.clear();
        metadata.shutdown();
    } };
}
module.exports = { create, CAPABILITY, CATALOG_REQUEST, CATALOG_RESPONSE, REQUEST, RESPONSE,
    REALM_REQUEST, REALM_RESPONSE, ANNOTATED_REQUEST, ANNOTATED_RESPONSE, ROW_BYTES, HEADER_BYTES, DIRECT, GATEWAY };
