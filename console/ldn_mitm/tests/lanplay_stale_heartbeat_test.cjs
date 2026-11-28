'use strict';
const assert = require('node:assert/strict');
const {Native, router, until, delay} = require('./lanplay_multi_test.cjs');
const L = require('../../../server/relay/lanplay');

async function run(replay) {
    const relay = await router();
    const host = new Native(relay.port, 0, process.argv[2]);
    const guest = new Native(relay.port, 1, process.argv[2]);
    let captured, hostEndpoint;
    relay.sock.on('message', (wire, from) => {
        if (wire[0] !== 1) return;
        const packet = L.parseIpv4(wire.subarray(1));
        if (!packet) return;
        if (packet.src === host.ip) hostEndpoint = from;
        if (packet.src === guest.ip && packet.proto === 17 && packet.id === 0x4243 &&
            packet.body.length >= 20 && packet.body.readUInt16BE(2) === 11452 &&
            L.ldnRead({}, packet.body.subarray(8)).some(frame => frame.type === 4))
            captured = Buffer.from(wire);
    });
    try {
        await Promise.all([host.ready(), guest.ready()]);
        host.cmd('HOST 0x0100123456789000 3');
        await host.wait(() => host.last('host'), 'host');
        guest.cmd(`JOIN 0x0100123456789000 ${host.ip} 1`);
        await guest.wait(() => guest.last('members')?.count === 2, 'initial standard membership');
        await until(() => captured && hostEndpoint, 'actual native heartbeat', 5000);
        const oldEpoch = host.last('host').epoch;
        host.cmd('LEAVE');
        await host.wait(() => host.last('left'), 'host leave');
        await guest.wait(() => guest.events.some(e => e.event === 'disconnected' && !e.active),
            'guest disconnect');
        guest.cmd('LEAVE');
        await guest.wait(() => guest.last('left'), 'guest cleanup');
        host.cmd('HOST 0x0100123456789000 3');
        await host.wait(() => host.last('host').epoch !== oldEpoch, 'host reopen');
        // Delay an actual valid packet across the room boundary. The BSSID is
        // unchanged, but the former member has not sent the new room's CONNECT.
        if (replay) relay.sock.send(captured, hostEndpoint.port, hostEndpoint.address);
        await delay(80);
        const start = guest.events.length;
        guest.cmd(`JOIN 0x0100123456789000 ${host.ip} 1`);
        await guest.wait(() => guest.events.slice(start).some(e => e.event === 'join'), 'rejoin result');
        assert(guest.last('join').ok);
        await guest.wait(() => guest.events.slice(start).some(e => e.event === 'members' &&
            e.count === 2 && e.ips.includes(guest.ip)), 'fresh rejoin admission', 5000);
        guest.cmd(`UDP ${host.ip} 30 64`);
        await host.wait(() => host.events.some(e => e.event === 'udp' && e.peer === guest.ip &&
            e.id === 30 && e.size === 64), 'rejoined guest data');
        await host.stop();
        await guest.stop();
        console.log(`PASS standard native guest rejoins after host reopen${replay ? ' with delayed prior heartbeat' : ' control'}`);
    } catch (error) {
        error.message += '\n' + JSON.stringify({host:host.report(), guest:guest.report()});
        throw error;
    } finally {
        for (const node of [host, guest]) if (!node.result) node.p.kill();
        relay.sock.close();
    }
}

(async () => { await run(false); await run(true); })().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
