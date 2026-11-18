// SPDX-License-Identifier: GPL-2.0-only
'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');

const anyIp = Buffer.from([255, 255, 255, 255]);

function uint(value, bytes) {
    const result = Buffer.alloc(bytes);
    result.writeUIntBE(value, 0, bytes);
    return result;
}

function text(value) {
    const bytes = Buffer.from(value);
    return Buffer.concat([uint(bytes.length, 4), bytes]);
}

// Uses genuine ENet client with independently encoded Eden v0.2.1 room packets.
function client(port, binaryDir = path.join(__dirname, "build")) {
    const child = spawn(path.resolve(binaryDir, 'enet-test-client'), ['127.0.0.1', String(port)]);
    let pending = Buffer.alloc(0), id;
    const messages = [];
    const closed = new Promise(resolve => child.once('close', resolve));

    child.stdout.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 9) {
            const length = pending.readUInt32BE(5);
            if (length + 9 > pending.length) return;
            if (pending[0] === 2) id = pending.readUInt32BE(1);
            if (pending[0] === 3) messages.push(pending.subarray(9, 9 + length));
            pending = pending.subarray(9 + length);
        }
    });
    child.stdin.on('error', () => {});
    function command(op, data = Buffer.alloc(0)) {
        assert(id, 'Client not connected');
        child.stdin.write(Buffer.concat([uint(op, 1), uint(id, 4), uint(data.length, 4), data]));
    }
    return {
        get connected() { return !!id; },
        messages,
        command,
        join(name, password = '', version = 1) {
            command(5, Buffer.concat([
                uint(1, 1), text(name), anyIp, uint(version, 4), text(password), text(''),
            ]));
        },
        close() { child.stdin.end(); return closed; },
    };
}

async function waitFor(fn, title) {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
        const result = fn();
        if (result) return result;
        await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.fail(`Timed out waiting for ${title}`);
}

module.exports = { client, uint, text, waitFor, anyIp };
