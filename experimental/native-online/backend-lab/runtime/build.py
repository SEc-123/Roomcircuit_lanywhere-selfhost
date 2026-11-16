#!/usr/bin/env python3
"""Build the independent RoomCircuit service from a pinned OpenPak NEX source."""

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN_SHA256 = "29c2e4661847fd1af81e0031d8e2c41db9a8974c3e73c921ab89f6882e7bafd2"
IDENTITY_SHA256 = "9e0ab6f825772d03ed100ce92842502f917bd5fc552fe32e6a82fe63675fc129"
TRANSPORT_SHA256 = "a4974a081cc8c9facaef0411cb0c9be9c4e6956bb808521941bded64909f52f6"
TEST_PATTERN = "^TestRoomCircuit(RuntimeConfiguration|RuntimeStartup|RuntimeProfiles|RuntimeTicketWindow|RuntimeSecureAdmission)$"


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Pinned source anchor changed: {old[:80]}")
    return source.replace(old, new, 1)


def pinned_source(path, digest):
    content = path.read_bytes()
    if hashlib.sha256(content).hexdigest() != digest:
        raise ValueError(f"{path.name} does not match OpenPak v0.3.45")
    return content.decode()


def patched_main(path):
    source = pinned_source(path, MAIN_SHA256)
    changes = [
        ('flag.String("station-address", "145.241.199.19",', 'flag.String("station-address", "",'),
        ('flag.Bool("require-identity", false,', 'flag.Bool("require-identity", true,'),
        ('var (\n\tstationAddress',
         'var (\n\tbindAddress = flag.String("bind-address", "127.0.0.1", "local IPv4 interface for game traffic")\n'
         '\tproxyPeer = flag.String("auth-proxy-peer", "", "optional exact trusted auth ingress IP")\n'
         '\tstationAddress'),
        ('\tlog.SetFlags(log.LstdFlags | log.LUTC)\n',
         '\tlog.SetFlags(log.LstdFlags | log.LUTC)\n'
         '\tidentity := orEnv(*identityURL, "NX_INTERNAL_URL")\n'
         '\tkey := orEnv(*identityKey, "NX_INTERNAL_KEY")\n'
         '\tticketSecret, err := runtimeConfiguration(*requireIdentity, identity, key, os.Getenv("RC_NEX_TICKET_KEY"),\n'
         '\t\t*stationAddress, *bindAddress, *proxyPeer, *only)\n'
         '\tif err != nil { log.Fatalf("configuration: %v", err) }\n'),
        ('MinVersion:   tls.VersionTLS10,', 'MinVersion:   tls.VersionTLS12,'),
        ('\t// Switch NEX clients negotiate legacy TLS (nn::ssl); the Go default\n'
         '\t// minimum of 1.2 rejects their ClientHello outright.\n',
         '\tif err := checkRuntimeCertificate(cert, *stationAddress); err != nil {\n'
         '\t\tlog.Fatalf("certificate does not cover this station: %v", err)\n\t}\n'
         '\t// Keep the independent realm at TLS 1.2 or newer.\n'),
        ('\tids := newIdentity(\n\t\torEnv(*identityURL, "NX_INTERNAL_URL"),\n'
         '\t\torEnv(*identityKey, "NX_INTERNAL_KEY"),\n\t)\n',
         '\tids := newIdentity(identity, key)\n'),
        ('svc := newService(t, *stationAddress, recall, ids)',
         'svc := newService(t, *stationAddress, recall, ids, ticketSecret)'),
        ('serveHealth(fmt.Sprintf(":%d", t.base+2), svc, voice)',
         'serveHealth(net.JoinHostPort("127.0.0.1", strconv.Itoa(t.base+2)), svc, voice)'),
        ('listenTitle(fmt.Sprintf(":%d", t.base), tlsCfg.Clone())',
         'listenRuntimeTitle(net.JoinHostPort(*bindAddress, strconv.Itoa(t.base)), tlsCfg.Clone(), *proxyPeer)'),
        ('listenTitle(fmt.Sprintf(":%d", t.base+1), tlsCfg.Clone())',
         'listenRuntimeTitle(net.JoinHostPort(*bindAddress, strconv.Itoa(t.base+1)), tlsCfg.Clone(), "")'),
        ('log.Printf("[%s] auth listen :%d: %v", t.name, t.base, err)\n\t\t\tcontinue',
         'log.Fatalf("[%s] auth listen :%d: %v", t.name, t.base, err)'),
        ('log.Printf("[%s] secure listen :%d: %v", t.name, t.base+1, err)\n'
         '\t\t\t// Half a title is no title: a login would hand out a secure\n'
         '\t\t\t// station nobody answers. Release the auth port too.\n'
         '\t\t\tauthLn.Close()\n\t\t\tcontinue',
         '_ = authLn.Close()\n\t\t\tlog.Fatalf("[%s] secure listen :%d: %v", t.name, t.base+1, err)'),
        ('func newService(t title, stationAddress string, recall *recall, ids *identity) *service {',
         'func newService(t title, stationAddress string, recall *recall, ids *identity, ticketSecret ...[]byte) *service {'),
        ('targetKey := switchnex.DeriveKey([]byte(securePassword), env.SecurePID)',
         'password := []byte(securePassword) // Archived upstream test fixtures call without a runtime secret.\n'
         '\tif len(ticketSecret) > 0 { password = runtimeTicketPassword(ticketSecret[0], t.name) }\n'
         '\ttargetKey := switchnex.DeriveKey(password, env.SecurePID)'),
        ('\t// The secure endpoint is also the path the server originates on:',
         '\tif len(ticketSecret) > 0 {\n'
         '\t\ts.secure.AuthorizeTicket = func(ticket *switchnex.ServerTicket) error {\n'
         '\t\t\treturn validateRuntimeTicket(ticket, ids)\n\t\t}\n\t}\n'
         '\t// The secure endpoint is also the path the server originates on:'),
        ('if !ids.enabled() {\n\t\treturn pidFor(username), 0, true\n\t}',
         'if !ids.enabled() {\n'
         '\t\tif *requireIdentity { return 0, rmc.ResultAuthValidationFail, false }\n'
         '\t\treturn pidFor(username), 0, true\n\t}'),
        ('log.Printf("[%s] healthz listen %s: %v", s.env.Name, addr, err)\n\t\treturn',
         'log.Fatalf("[%s] healthz listen %s: %v", s.env.Name, addr, err)'),
    ]
    for old, new in changes:
        source = replace_once(source, old, new)
    return source


def patched_identity(path):
    source = pinned_source(path, IDENTITY_SHA256)
    source = replace_once(source, 'client: &http.Client{Timeout: 5 * time.Second},',
        'client: &http.Client{Timeout: 5 * time.Second,\n'
        '\t\t\tCheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},')
    source = replace_once(source,
        '// stillAllowed re-asks nx-baas about a player who logged in earlier. It\n'
        '// returns errBanned for a locked-out account, nil for one still allowed, and\n'
        '// any other error when nx-baas could not answer (the session is kept then).',
        '// stillAllowed resolves every ticket principal against the independent live admission,\n'
        '// including after the game process restarts and its local map is empty.')
    source = replace_once(source,
        '\tv, ok := i.accounts.Load(nexPID)\n\tif !ok {\n\t\treturn nil // not a proven login: nothing to ask about\n\t}\n'
        '\t_, err := i.post(map[string]any{"pid": v.(uint64)})\n\treturn err',
        '\tif !i.enabled() { return errNoAccount }\n'
        '\tpid := nexPID\n'
        '\tif known, ok := i.accounts.Load(nexPID); ok { pid = known.(uint64) }\n'
        '\tplayer, err := i.post(map[string]any{"pid": pid})\n'
        '\tif err != nil { return err }\n'
        '\tif player.nexPID() != nexPID { return errNoAccount }\n'
        '\treturn nil')
    source = replace_once(source,
        '// watchBans drops secure connections whose account nx-baas now refuses as\n'
        '// banned. Only a positive "banned" drops a session; an nx-baas outage keeps\n'
        '// it. The console\'s next login is refused with RendezVous::AccountDisabled.',
        '// watchBans drops secure connections after a definitive revoked or unknown\n'
        '// admission. A temporary identity outage keeps an established connection.')
    source = replace_once(source, 'banned = errors.Is(i.stillAllowed(pid), errBanned)',
        'err := i.stillAllowed(pid)\n\t\t\tbanned = errors.Is(err, errBanned) || errors.Is(err, errNoAccount)')
    return replace_once(source, 'pid=%s is banned, dropping its connection', 'pid=%s has no active admission, dropping its connection')


def patched_transport(path):
    source = pinned_source(path, TRANSPORT_SHA256)
    source = replace_once(source, '\tKerberosKey []byte\n',
        '\tKerberosKey []byte\n'
        '\t// AuthorizeTicket checks authenticated ticket time and live authorization before CONNECT ACK.\n'
        '\tAuthorizeTicket func(*switchnex.ServerTicket) error\n')
    return replace_once(source, '\t}\n\tc.PID = pid\n\tc.CID = binary.LittleEndian.Uint32(dec[8:12])',
        '\t}\n\tif c.Endpoint.AuthorizeTicket != nil {\n'
        '\t\tif err := c.Endpoint.AuthorizeTicket(st); err != nil { return nil, err }\n'
        '\t}\n\tc.PID = pid\n\tc.CID = binary.LittleEndian.Uint32(dec[8:12])')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--test-binary", type=Path, help="Build a matching static test client")
    parser.add_argument("--linux", action="store_true", help="Build a static Linux x86-64 executable")
    parser.add_argument("--test", action="store_true", help="Run local runtime configuration tests")
    parser.add_argument("--credentials", type=Path, help="Run the local two-account production-profile test")
    args = parser.parse_args()
    if args.linux and (args.test or args.credentials):
        parser.error("running the runtime tests needs a host-native executable")
    source = args.source.resolve()
    binary = args.binary.resolve()
    command = source / "cmd" / "switch-nexd"
    environment = os.environ.copy()
    environment.update(GOWORK="off", GOTOOLCHAIN="local", GOPROXY="off", CGO_ENABLED="0")
    binary.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="roomcircuit-runtime-") as scratch_dir:
        scratch = Path(scratch_dir)
        overlay = scratch / "overlay.json"
        replacements = {str(command / "roomcircuit_policy.go"): str(HERE / "policy.go")}
        for original, patch in ((command / "main.go", patched_main), (command / "identity.go", patched_identity),
                                (source / "prudplite" / "server.go", patched_transport)):
            changed = scratch / original.name
            changed.write_text(patch(original))
            replacements[str(original)] = str(changed)
        subprocess.run(["gofmt", "-w", *[str(scratch / n) for n in ("main.go", "identity.go", "server.go")]], check=True)
        overlay.write_text(json.dumps({"Replace": replacements}))
        target = {"GOOS": "linux", "GOARCH": "amd64"} if args.linux else {}
        subprocess.run(["go", "build", f"-overlay={overlay}", "-trimpath", "-buildvcs=false", "-o", str(binary), "./cmd/switch-nexd"],
                       cwd=source, env={**environment, **target}, check=True)
        if args.test or args.credentials or args.test_binary:
            replacements[str(command / "roomcircuit_policy_test.go")] = str(HERE / "policy_test.go")
            replacements[str(command / "roomcircuit_runtime_test.go")] = str(HERE / "runtime_test.go")
            replacements[str(command / "roomcircuit_authenticated_room_test.go")] = str(HERE.parent / "authenticated_room_test.go")
            overlay.write_text(json.dumps({"Replace": replacements}))
        if args.test_binary:
            target_test = args.test_binary.resolve()
            target_test.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(["go", "test", f"-overlay={overlay}", "-trimpath", "-buildvcs=false", "-c",
                            "-o", str(target_test), "./cmd/switch-nexd"], cwd=source,
                           env={**environment, **target}, check=True)
        if args.test or args.credentials:
            environment.pop("RC_NEX_RUNTIME_CREDENTIALS", None)
            if args.credentials:
                environment["RC_NEX_RUNTIME_CREDENTIALS"] = str(args.credentials.resolve())
            environment["RC_NEX_RUNTIME_BINARY"] = str(binary)
            subprocess.run(["go", "test", f"-overlay={overlay}", "-count=1", "-v", "-timeout=60s", "./cmd/switch-nexd",
                            "-run", TEST_PATTERN],
                           cwd=source, env=environment, check=True)


if __name__ == "__main__":
    main()
