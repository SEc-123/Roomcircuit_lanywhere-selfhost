'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const wire = require('./update-format');

function create({ directory, send, error, maxTransfers = 8, maxPerIp = 2, intervalMs = 64,
    transferDeadlineMs = 10 * 60 * 1000 }) {
    if (directory !== undefined && (!path.isAbsolute(directory) || directory.includes('\0')))
        throw new Error('LANYWHERE_UPDATE_DIR must be an absolute directory');
    const sessions = new Map(), perIp = new Map(), updateSockets = new WeakSet();
    let activeTransfers = 0, reads = 0, closed = false, paused = false;

    async function openRegular(filename, expectedBytes) {
        const file = await fs.promises.open(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
        try {
            const stat = await file.stat();
            if (!stat.isFile() || stat.size !== expectedBytes) throw new Error('Invalid update file');
            return file;
        } catch (failure) {
            await file.close();
            throw failure;
        }
    }

    async function manifest() {
        const file = await openRegular(path.join(directory, 'current.manifest'), wire.MANIFEST_BYTES);
        try {
            const data = Buffer.alloc(wire.MANIFEST_BYTES);
            const result = await file.read(data, 0, data.length, 0);
            if (result.bytesRead !== data.length) throw new Error('Truncated update manifest');
            return { data, info: wire.parseManifest(data) };
        } finally { await file.close(); }
    }

    function alive(socket, session) {
        return !closed && !paused && !session.closed && !socket.destroyed && !socket.writableEnded;
    }

    function close(socket) {
        const session = sessions.get(socket);
        if (!session || session.closed) return;
        session.closed = true;
        sessions.delete(socket);
        if (session.reserved) {
            activeTransfers--;
            const remaining = perIp.get(session.ip) - 1;
            if (remaining) perIp.set(session.ip, remaining);
            else perIp.delete(session.ip);
        }
        if (session.file) void session.file.close().catch(() => {});
    }

    async function execute(socket, session, type, body) {
        try {
            if (type === wire.MANIFEST_REQUEST) {
                const current = await manifest();
                if (alive(socket, session)) send(socket, wire.MANIFEST_RESPONSE, current.data);
                return;
            }
            const hash = body.subarray(0, 32), offset = body.readUInt32LE(32);
            if (!session.file) {
                const current = await manifest();
                if (!current.info.hash.equals(hash) || offset >= current.info.compressedBytes)
                    return error(socket, 11, 'Update changed. Check again.');
                if (!alive(socket, session)) return;
                if (activeTransfers >= maxTransfers || (perIp.get(session.ip) || 0) >= maxPerIp)
                    return error(socket, 7, 'Update downloads are busy. Try again.');
                session.reserved = true;
                activeTransfers++;
                perIp.set(session.ip, (perIp.get(session.ip) || 0) + 1);
                const file = await openRegular(path.join(directory, hash.toString('hex') + '.lup'), current.info.compressedBytes);
                if (!alive(socket, session)) { await file.close(); return; }
                session.file = file;
                session.info = current.info;
                session.startedAt = Date.now();
            }
            if (!session.info.hash.equals(hash) || offset >= session.info.compressedBytes)
                return error(socket, 4, 'Invalid update chunk.');
            if (Date.now() - session.startedAt > transferDeadlineMs ||
                session.bytesSent >= session.info.compressedBytes * 2) {
                close(socket);
                return error(socket, 7, 'Update download expired. Retry.');
            }
            const waitMs = Math.max(0, session.nextAt - Date.now());
            if (waitMs) await delay(waitMs);
            if (!alive(socket, session)) return;
            const length = Math.min(wire.CHUNK_BYTES, session.info.compressedBytes - offset);
            const response = Buffer.alloc(4 + length);
            response.writeUInt32LE(offset);
            const result = await session.file.read(response, 4, length, offset);
            if (result.bytesRead !== length) throw new Error('Truncated update package');
            if (!alive(socket, session)) return;
            session.nextAt = Date.now() + intervalMs;
            session.bytesSent += length;
            send(socket, wire.CHUNK_RESPONSE, response);
        } catch {
            close(socket);
            if (!socket.destroyed && !socket.writableEnded) error(socket, 11, 'Update is unavailable. Try again.');
        } finally {
            session.pending = false;
            reads--;
        }
    }

    function handle(socket, type, body) {
        if (type !== wire.MANIFEST_REQUEST && type !== wire.CHUNK_REQUEST) return false;
        if (!directory || closed || paused) { error(socket, 11, 'Online updates are unavailable.'); return true; }
        if (body.length !== (type === wire.MANIFEST_REQUEST ? 0 : 36)) {
            error(socket, 4, 'Invalid update request.'); return true;
        }
        updateSockets.add(socket);
        let session = sessions.get(socket);
        if (!session) {
            session = { ip: (socket.remoteAddress || '').replace(/^::ffff:/, ''), pending: false,
                closed: false, reserved: false, bytesSent: 0, nextAt: 0 };
            sessions.set(socket, session);
        }
        if (session.pending || reads >= 16) { error(socket, 7, 'Update request is busy.'); return true; }
        if (session.file && type === wire.MANIFEST_REQUEST) {
            error(socket, 4, 'Finish the current download first.'); return true;
        }
        session.pending = true;
        reads++;
        void execute(socket, session, type, Buffer.from(body));
        return true;
    }

    return { handle, has: socket => updateSockets.has(socket), close,
        pause() { paused = true; for (const socket of sessions.keys()) close(socket); },
        resume() { if (!closed) paused = false; },
        shutdown() { closed = true; for (const socket of sessions.keys()) close(socket); },
        snapshot: () => ({ sessions: sessions.size, transfers: activeTransfers, reads }) };
}

module.exports = { create, ...wire };
