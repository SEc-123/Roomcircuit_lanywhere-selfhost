'use strict';

const SIZE = 16, CAPABILITY = 1048576, LIST_REQUEST = 0x3a, LIST_RESPONSE = 0xba, SET_REQUEST = 0x3b;

function parse(bytes, allowEmpty = false) {
    if (!Buffer.isBuffer(bytes) || bytes.length !== SIZE) return null;
    const end = bytes.indexOf(0);
    if (end < 0 || bytes.subarray(end).some(Boolean)) return null;
    if (!end) return allowEmpty ? '' : null;
    const text = bytes.subarray(0, end).toString('latin1');
    return /^[vV]?[0-9]/.test(text) && !/[^A-Za-z0-9._+-]/.test(text) ? text : null;
}

module.exports = { SIZE, CAPABILITY, LIST_REQUEST, LIST_RESPONSE, SET_REQUEST, parse };
