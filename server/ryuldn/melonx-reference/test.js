// SPDX-License-Identifier: GPL-2.0-only
'use strict';
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const path = require('node:path');
const { session } = require('../../../tools/lanplay-tun/reference/test-nx');
const published = process.env.MELONX_REFERENCE_BUILD;
if (!published) throw new Error('Set MELONX_REFERENCE_BUILD to the result of prepare.py');
const tools = path.resolve(__dirname, '../../../tools/lanplay-tun');
async function runDevice(box, role, port, echo = false) {
    const process = spawn('docker', ['run','--rm','--name',box,'--cap-add','NET_ADMIN','--device','/dev/net/tun',
        '--add-host','host.docker.internal:host-gateway','-e','RC_CLIENT_IP='+(echo?'10.13.250.6':'10.13.250.2'),
        '-e','RC_LANPLAY_PORT='+port,'-v',tools+':/tools:ro','-v',__dirname+':/check:ro',
        '-v',path.resolve(published)+':/provider:ro','roomcircuit-melonx-reference:local',
        'sh','/check/run-device.sh',role,echo?'echo':'client'],{stdio:['ignore','pipe','pipe']});
    let output='';
    for (const stream of [process.stdout,process.stderr]) stream.on('data',d=>output+=d);
    const [code] = await once(process, 'close');
    if (code !== 0) throw new Error(output || 'MeloNX driver stopped unexpectedly');
}
(async () => {
    const families = (process.env.MELONX_PAIR_TARGETS || '').split(',').filter(Boolean);
    for (const family of families.length ? families : ['Switch','Ryu','Eden','Yuzu','NX','same-driver'])
        for (const role of ['guest','host']) await session(family, role, runDevice, 'MeloNX LDN');
})().catch(e=>{console.error(e);process.exitCode=1;});
