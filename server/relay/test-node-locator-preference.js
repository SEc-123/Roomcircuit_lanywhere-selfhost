'use strict';
const assert = require('node:assert/strict');
const {test} = require('node:test');
const {setTimeout: delay} = require('node:timers/promises');
const {standard, launch, directRow} = require('./test-node-real-locator');
const {ForkPeer} = require('./test-lanplay-fork');
const L = require('./lanplay');

test('direct locator preference never mutates a gateway session or accepts a different room', () => {
    const info = Buffer.alloc(0x480); info[0] = 9; info[16] = 3; info[32] = 2;
    const bridge = {info, fork: {raw: info}, host13: 0x0a0d1111, lastSeen: 100};
    const original = bridge.fork;
    L.recordDirectInfo(bridge, info, true, 100);
    L.recordDirectInfo(bridge, info, false, 200);
    assert.equal(L.preferredDirectInfo(bridge, 300).flavor, 1);
    assert.equal(bridge.fork, original);
    const other = Buffer.from(info); other[16]++;
    L.recordDirectInfo(bridge, other, false, 7000);
    L.recordDirectInfo(bridge, info, true, 7000);
    assert.equal(L.preferredDirectInfo(bridge, 7001).flavor, 2);
    assert.equal(bridge.fork, original);
    assert(L.preferredDirectInfo(bridge, 7001).info.equals(info));
});

test('unified directory prefers verified standard wire then falls back to fresh fork for the same room', {timeout: 25000}, async t => {
    const relay = await standard(t), central = await launch(t, relay, {fastLobby:true});
    const host = new ForkPeer(0x0a0d4444, 0xc0a80144, 'Dual host', relay);
    await host.start(); host.host(); t.after(() => host.close());
    const gateways = new Set();
    host.sock.on('message', bytes => {
        if (bytes[0] !== 1) return;
        const packet = L.parseIpv4(bytes.subarray(1));
        if (!packet || packet.proto !== 17 || packet.body.readUInt16BE(2) !== 11452) return;
        if (L.ldnRead({}, packet.body.subarray(8)).some(frame => frame.type === 0)) gateways.add(packet.src);
    });
    const browser = await central.peer('Directory');
    const game = host.info.readBigUInt64LE();
    const first = await directRow(browser, game);
    assert.equal(first[344 + 38], 2);
    const info = Buffer.from(host.info); info.writeUInt32LE(host.virt, 0x68);
    const announceStandard = () => {
        for (const gateway of gateways) host.raw(Buffer.concat([Buffer.from([1]),
            L.udpPacket(host.virt, 11452, gateway, 11452, L.ldnFrame(1, info))]));
    };
    const forkTimer = setInterval(() => host.control(1, host.info), 250);
    let standardTimer = setInterval(announceStandard, 250);
    t.after(() => { clearInterval(forkTimer); clearInterval(standardTimer); });
    const waitFlavor = async expected => {
        for (let i = 0; i < 70; i++) {
            const row = await directRow(browser, game);
            if (row[344 + 38] === expected) return row;
            await delay(100);
        }
        throw Error(`locator did not become flavor ${expected}; gateways=${[...gateways]}; ${central.getLog()}`);
    };
    const preferred = await waitFlavor(1);
    assert(preferred.subarray(344, 382).equals(first.subarray(344, 382)));
    assert.equal(preferred.readUInt32LE(344 + 40), host.virt);
    clearInterval(standardTimer); standardTimer = null;
    const fallback = await waitFlavor(2);
    assert(fallback.subarray(344, 382).equals(first.subarray(344, 382)));
});
