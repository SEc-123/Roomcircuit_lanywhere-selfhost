#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual emulator socket class and run its gameplay socket regression."""
from pathlib import Path
import argparse
import contextlib
import os
import socket
import time
import shutil
import subprocess
import tempfile

PINS = {'6af327bdd50366163d80bc692f6bf47ec1c1993e',
        '506e2c7a69b4f801a519857e465e81834d618da8',
        '280c427c550c5fe3bcc46b378e06a9b90a7e6b09b'}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('checkout', type=Path)
parser.add_argument('commit', choices=sorted(PINS))
parser.add_argument('--docker', action='store_true')
args = parser.parse_args()
src = Path(__file__).resolve().parent
base = 'src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnRyu'
files = ['Proxy/LdnProxySocket.cs'] + ['Types/' + t + '.cs' for t in
    ['ProxyInfo', 'ProxyConnectRequest', 'ProxyConnectResponse', 'ProxyDisconnectMessage',
     'ProxyDataPacket', 'ProxyDataHeader']]
with tempfile.TemporaryDirectory() as tmp:
    dest = Path(tmp)
    for name in files:
        rel = base + '/' + name
        p = dest / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(subprocess.check_output(['git', '-C', str(args.checkout.resolve()), 'show',
                                              args.commit + ':' + rel]))
    subprocess.run(['git', 'init', '-q'], cwd=dest, check=True)
    subprocess.run(['git', 'apply', str(src.parent / 'ryu-client-sockets.patch')], cwd=dest, check=True)
    for source in src.glob('*.cs'):
        shutil.copyfile(source, dest / source.name)
    (dest / 'SocketTest.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
        '<OutputType>Exe</OutputType><TargetFramework>net9.0</TargetFramework>'
        '<RestoreIgnoreFailedSources>true</RestoreIgnoreFailedSources></PropertyGroup></Project>')
    repo = src.parents[2]
    with contextlib.ExitStack() as cleanup:
        port = 30456
        if args.docker:
            container = subprocess.check_output(['docker','run','-d','--rm','--network','none',
                '-v',f'{repo}:/app:ro','-w','/app','-e','ROOMCIRCUIT_RYULDN_PORT=30456',
                '-e','LANYWHERE_PUBLIC_PORT=0','-e','LANYWHERE_STATS_PORT=0','-e','LANYWHERE_STATE_DIR=/tmp/rc-state',
                'node:24-bookworm-slim','node','server/relay/server.js','--port','0'], text=True).strip()
            cleanup.callback(subprocess.run, ['docker','stop','-t','1', container], stdout=subprocess.DEVNULL)
            runtime = ['docker','run','--rm','--network',f'container:{container}','-v',f'{dest}:/source',
                       '-w','/source','mcr.microsoft.com/dotnet/sdk:9.0']
        else:
            runtime = []
            with socket.socket() as ephemeral:
                ephemeral.bind(('127.0.0.1',0))
                port = ephemeral.getsockname()[1]
            relay = subprocess.Popen(['node','server/relay/server.js','--port','0'], cwd=repo,
                env=dict(os.environ, ROOMCIRCUIT_RYULDN_PORT=str(port), ROOMCIRCUIT_EMULATOR_BIND='127.0.0.1',
                         LANYWHERE_STATS_PORT='0', LANYWHERE_PUBLIC_PORT='0', LANYWHERE_STATE_DIR=str(dest/'state')))
            cleanup.callback(relay.wait, timeout=5)
            cleanup.callback(relay.terminate)
            until = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(('127.0.0.1',port),0.3): break
                except ConnectionRefusedError:
                    if relay.poll() is not None or time.monotonic() >= until: raise
                    time.sleep(0.05)
        subprocess.run(runtime + ['dotnet','run','--project','SocketTest.csproj','--',str(port)],
                       cwd=dest, check=True, timeout=120)
