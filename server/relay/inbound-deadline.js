'use strict';

const { SUBSCRIBE, REQUEST_BYTES } = require('./appointment-push');
const { REQUEST: REGIONAL_LOBBY } = require('./regional-directory');
const { MANIFEST_REQUEST, CHUNK_REQUEST } = require('./update-format');

// Framing is checked by the caller before this function; command-specific
// validation remains in the normal protocol handlers.
function knownCommand(type, body) {
    if (type === 0x32) return body.length === 0;
    if (type === 0x33 || type === 0x37) return body.length === 20;
    if (type === 0x34) return body.length === 240;
    if (type === 0x35) return body.length === 12;
    if (type === 0x36) return body.length === 52 || body.length === 384;
    if (type === MANIFEST_REQUEST) return body.length === 0;
    if (type === CHUNK_REQUEST) return body.length === 36;
    return ((type >= 1 && type <= 0x0d) || type === 0x20 || type === 0x21 ||
            type === 0x22 || type === 0x24 || type === 0x26 || (type === 0x27 && body.length === 48) ||
            ((type === 0x28 || type === REGIONAL_LOBBY) && body.length === 8) ||
            (type === SUBSCRIBE && body.length === REQUEST_BYTES)) && (type !== 5 || body.length === 0);
}

function create(socket, timeoutMs) {
    let timer;
    function touch() {
        clearTimeout(timer);
        timer = setTimeout(() => socket.destroy(), timeoutMs);
        timer.unref();
    }
    function close() { clearTimeout(timer); timer = undefined; }
    touch();
    return { touch, close };
}

module.exports = { create, knownCommand };
