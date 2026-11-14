'use strict';

const MANIFEST_BYTES = 512, SIGNED_BYTES = 448, SIGNATURE_BYTES = 64;
const MAX_COMPRESSED_BYTES = 8 * 1024 * 1024, MAX_RAW_BYTES = 16 * 1024 * 1024;
const CHUNK_BYTES = 32768, MANIFEST_REQUEST = 0x30, CHUNK_REQUEST = 0x31;
const MANIFEST_RESPONSE = 0xb0, CHUNK_RESPONSE = 0xb1;

function parseManifest(bytes) {
    if (!Buffer.isBuffer(bytes) || bytes.length !== MANIFEST_BYTES || bytes.toString('ascii', 0, 4) !== 'LNU1' ||
        bytes.readUInt32LE(4) !== 1 || bytes.readUInt32LE(20) !== 1 || bytes.subarray(388, SIGNED_BYTES).some(Boolean))
        throw new Error('Invalid update manifest');
    const compressedBytes = bytes.readUInt32LE(24), rawBytes = bytes.readUInt32LE(28), files = bytes.readUInt32LE(32);
    if (!compressedBytes || compressedBytes > MAX_COMPRESSED_BYTES || rawBytes < 12 || rawBytes > MAX_RAW_BYTES ||
        !files || files > 64 || [8, 12, 16].some(offset => bytes.readUInt32LE(offset) > 65535))
        throw new Error('Invalid update manifest limits');
    for (const offset of [68, 228]) {
        const field = bytes.subarray(offset, offset + 160), end = field.indexOf(0);
        if (end < 0 || field.subarray(end).some(Boolean)) throw new Error('Invalid update note');
        new TextDecoder('utf-8', { fatal: true }).decode(field.subarray(0, end));
    }
    return { compressedBytes, rawBytes, files, hash: Buffer.from(bytes.subarray(36, 68)),
        version: [8, 12, 16].map(offset => bytes.readUInt32LE(offset)).join('.') };
}

module.exports = { MANIFEST_BYTES, SIGNED_BYTES, SIGNATURE_BYTES, MAX_COMPRESSED_BYTES, MAX_RAW_BYTES,
    CHUNK_BYTES, MANIFEST_REQUEST, CHUNK_REQUEST, MANIFEST_RESPONSE, CHUNK_RESPONSE, parseManifest };
