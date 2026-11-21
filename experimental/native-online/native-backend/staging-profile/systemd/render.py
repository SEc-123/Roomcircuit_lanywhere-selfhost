#!/usr/bin/env python3
"""Render isolated, loopback-only systemd units. Never install or start them."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

SERVICE_USER = 'roomcircuit-native-stage'
PREFIX = 'roomcircuit-native-stage-'
TITLES = {'mario-party-superstars': '01006fe013472000', 'pico-park': '01004ea00df70000'}
# Per-process caps, not proof of an adequate whole-machine memory budget.
CAPS_MIB = {'postgres': 72, 'core': 64, 'identity': 32, 'broker': 48, 'adapter': 48, 'register': 32, 'nex': 64}
CPU = {'postgres': 60, 'core': 80, 'identity': 30, 'broker': 50, 'adapter': 50, 'register': 30, 'nex': 70}
GO_HEAP = {'core': 40, 'identity': 20, 'broker': 32, 'adapter': 32, 'register': 20, 'nex': 40}
PARENTS = {'postgres': None, 'core': 'postgres', 'identity': 'core', 'broker': 'identity', 'adapter': 'broker', 'register': 'broker', 'nex': 'adapter'}
BIN = {'postgres': 'pg-stage', 'core': 'username-core', 'identity': 'identity', 'broker': 'broker', 'adapter': 'adapter', 'register': 'register-gateway', 'nex': 'nex'}


def check_stage(stage):
    if not isinstance(stage, str) or not stage.startswith('/') or stage == '/' or any(x in stage for x in ('\x00', '\n', '\r', '"', '\\', '%', '@')):
        raise ValueError('stage must be an absolute, non-root path without systemd specifiers or unsafe characters')
    p = Path(stage)
    if '..' in p.parts or '.' in stage.split('/') or str(p) != stage.rstrip('/'):
        raise ValueError('stage must be a normalized absolute path')
    # No specifier, shell, or unit-file metacharacters can be interpolated.
    if not re.fullmatch(r'/[A-Za-z0-9._/-]+', stage):
        raise ValueError('stage contains unsupported path characters')
    return str(p)


def command(stage, role, title):
    def address(port): return f'127.0.0.1:{port}'
    def url(port): return f'http://{address(port)}'
    def https(port): return f'https://{address(port)}'
    title_id = TITLES[title]
    values = {
        'postgres': {'RC_STAGE_PG_ROOT':stage, 'RC_STAGE_PG_PORT':'39900'},
        'core': {'RC_USERNAME_HTTP':address(39910), 'RC_USERNAME_GRPC':address(39911)},
        'identity': {'RC_CORE_HTTP':url(39910), 'RC_CORE_GRPC':address(39911), 'RC_IDENTITY_LISTEN':address(39912), 'RC_ACCOUNT_REALM':'username'},
        'broker': {'RC_ACCOUNT_URL':url(39910), 'RC_LINKS_URL':url(39912), 'RC_DEVICE_ISSUER':https(39913), 'RC_DEVICE_LISTEN':address(39913), 'RC_DEVICE_INTERNAL_LISTEN':address(39914), 'RC_ACCOUNT_CORE_GRPC':address(39911), 'RC_ACCOUNT_REALM':'username', 'RC_DEVICE_APPLICATIONS':title_id},
        'adapter': {'NX_LAB_ACCOUNT_REALM':'username', 'NX_LAB_APPLICATION':title_id, 'NX_LAB_LISTEN':address(39915), 'NX_LAB_INTERNAL':address(39916), 'NX_LAB_ISSUER':https(39915), 'NX_LAB_NA_ISSUER':https(39915), 'NX_LAB_ACCOUNT_URL':url(39910), 'NX_LAB_GRANT_URL':url(39914), 'NX_LAB_BROWSER_ORIGIN':https(39913), 'NX_LAB_DEVICE_HOST':''},
        'register': {'RC_REGISTER_LISTEN':address(39917), 'RC_REGISTER_ORIGIN':https(39917), 'RC_REGISTER_UPSTREAM':https(39913)},
        'nex': {'NX_INTERNAL_URL':url(39916)},
    }[role]
    if role in GO_HEAP:
        values['GOMEMLIMIT'] = f'{GO_HEAP[role]}MiB'
    args = [f'{k}={v}' for k, v in values.items()] + [f'{stage}/bin/{BIN[role]}']
    if role == 'postgres': args += ['serve']
    if role == 'nex': args += [f'-only={title}', '-station-address=127.0.0.1', '-bind-address=127.0.0.1', f'-cert={stage}/tls/server.pem', f'-key={stage}/tls/server.key']
    return '/usr/bin/env ' + ' '.join('"' + arg + '"' for arg in args)


def unit(stage, title, role):
    parent = PARENTS[role]
    dep = '' if parent is None else f'Wants={PREFIX}{parent}.service\nAfter={PREFIX}{parent}.service\n'
    envfile = 'postgres' if role == 'postgres' else role
    extra_env = f'EnvironmentFile={stage}/env/database.env\n' if role == 'core' else ''
    # pg-stage checks permissions and can restore database.env after an
    # interrupted initial publication. Other services only read env files.
    writable = f'{stage}/state' + (f' {stage}/env' if role == 'postgres' else '')
    return f'''# Generated loopback-only experimental service; do not use as a public profile.
[Unit]
Description=RoomCircuit isolated native stage: {role}
{dep}StartLimitIntervalSec=300
StartLimitBurst=20

[Service]
Type=simple
User={SERVICE_USER}
Group={SERVICE_USER}
WorkingDirectory={stage}
EnvironmentFile={stage}/env/{envfile}.env
{extra_env}ExecStart={command(stage, role, title)}
Restart=on-failure
RestartSec=3s
TimeoutStopSec=20s
UMask=0077
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
LockPersonality=yes
RestrictSUIDSGID=yes
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
IPAddressDeny=any
IPAddressAllow=localhost
ReadWritePaths={writable}
MemoryMax={CAPS_MIB[role]}M
CPUQuota={CPU[role]}%
LimitCORE=0

[Install]
WantedBy={PREFIX}stack.target
'''


def render(stage, title, output):
    stage=check_stage(stage)
    if title not in TITLES:
        raise ValueError('unsupported title')
    output=Path(output)
    # Exclusive directory creation prevents overwriting pre-existing system units.
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    names=[]
    try:
        for role in PARENTS:
            name=PREFIX+role+'.service'
            names.append(name)
            with (output/name).open('x', encoding='utf-8') as f:
                f.write(unit(stage,title,role)); f.flush(); os.fsync(f.fileno())
        target=PREFIX+'stack.target'
        names.append(target)
        wants=' '.join(PREFIX+role+'.service' for role in PARENTS)
        (output/target).write_text(f'[Unit]\nDescription=RoomCircuit isolated native staging stack\nWants={wants}\nAfter=network.target\n\n[Install]\nWantedBy=multi-user.target\n')
        manifest={'scope':'loopback-only staging templates; not installed or started','service_user':SERVICE_USER,'stage':stage,
                  'title':title,'title_id':TITLES[title],'memory_max_mib':CAPS_MIB,'total_memory_max_mib':sum(CAPS_MIB.values()),
                  'units':{name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in names}}
        names.append('manifest.json')
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    except BaseException:
        for name in names:
            (output/name).unlink(missing_ok=True)
        output.rmdir()
        raise
    return manifest


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', required=True, help='Absolute staging root path')
    p.add_argument('--title', required=True, choices=sorted(TITLES))
    p.add_argument('--output', required=True, help='A new local directory; no files are installed to systemd')
    a=p.parse_args()
    try:
        m=render(a.stage,a.title,a.output)
    except (ValueError,OSError) as exc:
        p.error(str(exc))
    print('Rendered loopback-only staging units:',len(m['units']))

if __name__ == '__main__': main()
