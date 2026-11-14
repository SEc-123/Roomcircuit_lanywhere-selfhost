'use strict';

const SIZE = 64, SUMMARY_BYTES = 336, ROW_BYTES = SUMMARY_BYTES + SIZE;
function virtualIp(ip) { return (ip >>> 16) === 0x0a0d && (ip & 0xffff) !== 0 && (ip & 0xffff) !== 0xffff; }
function create(info, hostIp, flavor, code) {
    if (!Buffer.isBuffer(info) || info.length !== 0x480 || ![0,1,2].includes(flavor) ||
        !info.readBigUInt64LE() || !/^[A-HJ-NP-Z2-9]{6}$/.test(code)) return null;
    if (flavor && (!virtualIp(hostIp) || !info.subarray(0x10,0x20).some(Boolean) || !info.subarray(0x20,0x26).some(Boolean))) return null;
    const out = Buffer.alloc(SIZE);
    info.copy(out,0,0,32);
    info.copy(out,32,0x20,0x26);
    out[38] = flavor;
    out.writeUInt32LE(hostIp,40);
    out.write(code,44,6,'ascii');
    return out;
}
function valid(value, gameId, code, requireLanPlay) {
    if (!Buffer.isBuffer(value) || value.length !== SIZE || value[39] || value.subarray(50).some(Boolean) ||
        value.readBigUInt64LE() !== gameId || value.toString('ascii',44,50) !== code) return false;
    const flavor = value[38];
    if (requireLanPlay) return [1,2].includes(flavor) && virtualIp(value.readUInt32LE(40)) &&
        value.subarray(16,32).some(Boolean) && value.subarray(32,38).some(Boolean);
    return flavor === 0;
}
module.exports = { SIZE, SUMMARY_BYTES, ROW_BYTES, create, valid };
