// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	switchnex "github.com/openpak/switch-nex"
	"github.com/openpak/switch-nex/protocols"
	"github.com/openpak/switch-nex/prudplite"
	"github.com/openpak/switch-nex/rmc"
)

func TestRoomCircuitRuntimeStartup(t *testing.T) {
	binary := os.Getenv("RC_NEX_RUNTIME_BINARY")
	if binary == "" {
		t.Fatal("runtime binary path is required")
	}
	for _, setting := range []struct{ name, endpoint, key, ticket, want string }{
		{"missing identity URL", "", strings.Repeat("1a", 32), strings.Repeat("2b", 32), "identity URL"},
		{"missing identity key", "http://127.0.0.1:39283", "", strings.Repeat("2b", 32), "NX_INTERNAL_KEY"},
		{"missing private ticket secret", "http://127.0.0.1:39283", strings.Repeat("1a", 32), "", "RC_NEX_TICKET_KEY"},
	} {
		t.Run(setting.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, binary, "-station-address=127.0.0.1", "-only=pico-park")
			cmd.Env = append(os.Environ(), "NX_INTERNAL_URL="+setting.endpoint, "NX_INTERNAL_KEY="+setting.key,
				"RC_NEX_TICKET_KEY="+setting.ticket)
			output, err := cmd.CombinedOutput()
			if err == nil || ctx.Err() != nil || !strings.Contains(string(output), "configuration:") ||
				!strings.Contains(string(output), setting.want) || strings.Contains(string(output), "serving auth") {
				t.Fatalf("process did not fail closed before TLS or bind: %v", err)
			}
		})
	}
}

func TestRoomCircuitRuntimeProfiles(t *testing.T) {
	fixturePath := os.Getenv("RC_NEX_RUNTIME_CREDENTIALS")
	if fixturePath == "" {
		t.Skip("owned account lab fixture required")
	}
	fixture := readRuntimeFixture(t, fixturePath)
	tokens := []string{fixture.host, fixture.guest}
	pids := []uint64{fixture.hostPID, fixture.guestPID}
	process, clientTLS := newTestRuntime(t, "mario-party-superstars,pico-park", fixture.endpoint, fixture.key)
	process.start(t)

	for _, profile := range titles {
		if profile.name != "mario-party-superstars" && profile.name != "pico-park" {
			continue
		}
		t.Run(profile.name, func(t *testing.T) {
			waitRuntimeHealth(t, profile)
			if _, reply := runtimeLogin(t, clientTLS, profile, "invalid-test-credential"); !reply.IsError {
				t.Fatal("runtime minted a ticket for an invalid account token")
			}
			peers := make([]*client, len(pids))
			for i, pid := range pids {
				auth, reply := runtimeLogin(t, clientTLS, profile, tokens[i])
				if reply.IsError {
					t.Fatalf("owner %d authentication result %#x", i+1, reply.ErrorCode)
				}
				auth.readTicketResult(reply.Body)
				auth.openTicket()
				station := switchnex.Parse(auth.stationURL)
				if station.Scheme != "prudps" || net.JoinHostPort(station.Get("address"), station.Get("port")) != runtimeEndpoint(profile, 1) || auth.pid != pid {
					t.Fatal("auth returned the wrong authenticated identity or secure endpoint")
				}
				peer := runtimeSecurePeer(t, clientTLS, profile, auth)
				peer.handshake(peer.secureConnectPayload(uint32(i + 1)))
				peers[i] = peer
			}
			if profile.name == "mario-party-superstars" {
				auditMarioPartySettings(t, peers[0])
			}
			runtimeRoom(t, peers[0], peers[1], profile.name == "pico-park")
		})
	}
}

type testRuntime struct {
	profiles, endpoint, identityKey, ticketKey string
	certPath, keyPath                          string
}

func newTestRuntime(t *testing.T, profiles, endpoint, identityKey string) (testRuntime, *tls.Config) {
	t.Helper()
	serverTLS, clientTLS := ownTLS(t)
	certificate := serverTLS.Certificates[0]
	der, err := x509.MarshalPKCS8PrivateKey(certificate.PrivateKey)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	certPath, keyPath := filepath.Join(dir, "cert.pem"), filepath.Join(dir, "key.pem")
	if err := os.WriteFile(certPath, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certificate.Certificate[0]}), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0600); err != nil {
		t.Fatal(err)
	}
	return testRuntime{profiles, endpoint, identityKey, hex.EncodeToString(ownSecret(t, 32)), certPath, keyPath}, clientTLS
}

func (r testRuntime) start(t *testing.T) func() {
	t.Helper()
	logPath := filepath.Join(t.TempDir(), "runtime.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_EXCL, 0600)
	if err != nil {
		t.Fatal(err)
	}
	defer logFile.Close()
	cmd := exec.Command(os.Getenv("RC_NEX_RUNTIME_BINARY"), "-station-address=127.0.0.1",
		"-only="+r.profiles, "-cert="+r.certPath, "-key="+r.keyPath)
	cmd.Env = append(os.Environ(), "NX_INTERNAL_URL="+r.endpoint, "NX_INTERNAL_KEY="+r.identityKey,
		"RC_NEX_TICKET_KEY="+r.ticketKey)
	cmd.Stdout, cmd.Stderr = logFile, logFile
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	var stopped sync.Once
	stop := func() { stopped.Do(func() { _ = cmd.Process.Kill(); _ = cmd.Wait() }) }
	t.Cleanup(stop)
	return stop
}

func runtimeEndpoint(profile title, offset int) string {
	return fmt.Sprintf("127.0.0.1:%d", profile.base+offset)
}

func dialRuntime(t *testing.T, clientTLS *tls.Config, profile title, offset int) *client {
	t.Helper()
	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 3 * time.Second}, "tcp", runtimeEndpoint(profile, offset), clientTLS)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return &client{t: t, conn: conn, accessKey: profile.accessKey}
}

func runtimeLogin(t *testing.T, clientTLS *tls.Config, profile title, token string) (*client, *rmc.RMCMessage) {
	t.Helper()
	peer := dialRuntime(t, clientTLS, profile, 0)
	peer.handshake(nil)
	request := rmc.NewWriter()
	request.StructOf(1, func(fields *rmc.Writer) {
		fields.U32(3)
		fields.String("RoomCircuit test account")
		fields.Raw(authInfoHolder(token))
	})
	return peer, peer.call(0x0A, 0x06, request.Bytes())
}

func runtimeSecurePeer(t *testing.T, clientTLS *tls.Config, profile title, auth *client) *client {
	t.Helper()
	peer := dialRuntime(t, clientTLS, profile, 1)
	peer.pid, peer.ticket, peer.sessionKey = auth.pid, auth.ticket, auth.sessionKey
	return peer
}

func TestRoomCircuitRuntimeSecureAdmission(t *testing.T) {
	var profile title
	for _, candidate := range titles {
		if candidate.name == "pico-park" {
			profile = candidate
			break
		}
	}
	if profile.base == 0 {
		t.Fatal("PICO PARK profile unavailable")
	}
	const pid uint64 = 0x07a1
	key := hex.EncodeToString(ownSecret(t, 32))
	credential := hex.EncodeToString(ownSecret(t, 32))
	var shortTokenActive atomic.Bool
	shortTokenActive.Store(true)
	var admitted atomic.Bool
	var pidStatus, pidChecks atomic.Int32
	pidStatus.Store(http.StatusOK)
	authority := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/internal/switch/identity" || r.Header.Get("X-Internal-Key") != key {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		var body struct {
			IDToken string `json:"id_token"`
			PID     uint64 `json:"pid"`
		}
		if json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&body) != nil {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if body.PID != 0 {
			pidChecks.Add(1)
			if body.PID != pid || !admitted.Load() {
				w.WriteHeader(http.StatusNotFound)
				return
			}
			if status := int(pidStatus.Load()); status != http.StatusOK {
				w.WriteHeader(status)
				return
			}
		} else if body.IDToken != credential || !shortTokenActive.Load() {
			w.WriteHeader(http.StatusUnauthorized)
			return
		} else {
			admitted.Store(true)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"pid": pid, "baas_user_id": fmt.Sprintf("%016x", pid), "account_id": "owned-test-account"})
	}))
	t.Cleanup(authority.Close)
	process, clientTLS := newTestRuntime(t, profile.name, authority.URL, key)
	stop := process.start(t)
	waitRuntimeHealth(t, profile)
	auth, reply := runtimeLogin(t, clientTLS, profile, credential)
	if reply.IsError {
		t.Fatal("owned game login did not mint a ticket")
	}
	auth.readTicketResult(reply.Body)
	auth.openTicket()
	first := runtimeSecurePeer(t, clientTLS, profile, auth)
	first.handshake(first.secureConnectPayload(1))
	if pidChecks.Load() != 1 {
		t.Fatal("secure admission did not check the live lease before acknowledging")
	}

	shortTokenActive.Store(false)
	stop()
	process.start(t)
	waitRuntimeHealth(t, profile)
	pidStatus.Store(http.StatusNotFound)
	unknown := runtimeSecurePeer(t, clientTLS, profile, auth)
	assertRuntimeConnectRejected(t, unknown, 2)
	pidStatus.Store(http.StatusServiceUnavailable)
	unavailable := runtimeSecurePeer(t, clientTLS, profile, auth)
	assertRuntimeConnectRejected(t, unavailable, 3)
	pidStatus.Store(http.StatusOK)
	resumed := runtimeSecurePeer(t, clientTLS, profile, auth)
	resumed.handshake(resumed.secureConnectPayload(4))
	if pidChecks.Load() != 4 {
		t.Fatal("the restarted process did not consult live admission for each reconnect")
	}
	if _, expiredLogin := runtimeLogin(t, clientTLS, profile, credential); !expiredLogin.IsError {
		t.Fatal("the short credential still authorized a fresh login")
	}

	pidStatus.Store(http.StatusForbidden)
	revoked := runtimeSecurePeer(t, clientTLS, profile, auth)
	assertRuntimeConnectRejected(t, revoked, 5)
	if err := resumed.conn.SetReadDeadline(time.Now().Add(35 * time.Second)); err != nil {
		t.Fatal(err)
	}
	_, err := io.Copy(io.Discard, resumed.conn)
	if network, ok := err.(net.Error); ok && network.Timeout() {
		t.Fatal("the live secure connection survived a definitive authorization revocation")
	}
	if checks := pidChecks.Load(); checks < 6 {
		t.Fatal("the runtime disconnected without performing its periodic lease check")
	}
}

func assertRuntimeConnectRejected(t *testing.T, peer *client, cid uint32) {
	t.Helper()
	peer.send(&prudplite.Packet{Type: prudplite.Syn, SequenceID: 0})
	syn := peer.recv()
	if syn.Type != prudplite.Syn || !syn.HasFlag(prudplite.FlagAck) {
		t.Fatal("secure transport did not answer SYN")
	}
	connect := &prudplite.Packet{
		Type: prudplite.Connect, Flags: prudplite.FlagReliable | prudplite.FlagNeedsAck,
		SequenceID: 1, Payload: peer.secureConnectPayload(cid),
		SourceStreamType: prudplite.StreamState, SourceVirtualPort: 0x0F,
		DestinationStreamType: prudplite.StreamState, DestinationVirtualPort: 0x01,
	}
	connect.Signature = prudplite.LitePacketSignature(peer.accessKey, connect, syn.ConnectionSig)
	peer.send(connect)
	if refusal := peer.recv(); refusal.Type != prudplite.Disconnect {
		t.Fatal("secure connect was not rejected before acknowledgment")
	}
}

type runtimeFixture struct {
	endpoint, key, host, guest string
	hostPID, guestPID          uint64
}

func readRuntimeFixture(t *testing.T, path string) runtimeFixture {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal("cannot open local account fixture")
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
		t.Fatal("local account fixture must be an owner-only regular file")
	}
	owner, ok := info.Sys().(*syscall.Stat_t)
	if !ok || int(owner.Uid) != os.Geteuid() || info.Size() == 0 || info.Size() > 8192 {
		t.Fatal("local account fixture owner or size is invalid")
	}
	var stored struct {
		Endpoint string      `json:"RC_IDENTITY_URL"`
		Key      string      `json:"RC_IDENTITY_KEY"`
		Host     string      `json:"RC_HOST_TOKEN"`
		Guest    string      `json:"RC_GUEST_TOKEN"`
		HostPID  json.Number `json:"RC_HOST_PID"`
		GuestPID json.Number `json:"RC_GUEST_PID"`
	}
	body, err := io.ReadAll(io.LimitReader(file, 8193))
	if err != nil || len(body) > 8192 || json.Unmarshal(body, &stored) != nil {
		t.Fatal("local account fixture is invalid")
	}
	hostPID, hostError := strconv.ParseUint(stored.HostPID.String(), 10, 64)
	guestPID, guestError := strconv.ParseUint(stored.GuestPID.String(), 10, 64)
	if stored.Endpoint == "" || stored.Key == "" || stored.Host == "" || stored.Guest == "" || stored.Host == stored.Guest ||
		hostError != nil || guestError != nil || hostPID == 0 || guestPID == 0 || hostPID == guestPID {
		t.Fatal("local account fixture must contain two distinct authenticated identities")
	}
	return runtimeFixture{stored.Endpoint, stored.Key, stored.Host, stored.Guest, hostPID, guestPID}
}

func waitRuntimeHealth(t *testing.T, profile title) {
	t.Helper()
	httpClient := http.Client{Timeout: 200 * time.Millisecond}
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		response, err := httpClient.Get(fmt.Sprintf("http://127.0.0.1:%d/healthz", profile.base+2))
		if err == nil {
			_ = response.Body.Close()
			if response.StatusCode == http.StatusOK {
				return
			}
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatal("exact runtime profile listeners did not become ready")
}

func runtimeRoom(t *testing.T, host, guest *client, serverMakesKey bool) {
	t.Helper()
	station := rmc.NewWriter()
	station.List(1)
	station.String("prudp:/address=127.0.0.1;port=50000;sid=1;natf=0")
	if result := host.call(ProtoSecureConnection, 0x01, station.Bytes()); result.IsError {
		t.Fatalf("station registration result %#x", result.ErrorCode)
	}
	created := host.call(ProtoMatchmakeExt, 0x06, sessionHolderBytes(2, 3, 77))
	_, roomKey, err := readJoinedSession(created.Body)
	if created.IsError || err != nil || (serverMakesKey && len(roomKey) != 32) {
		t.Fatalf("create result %#x with key length %d: %v", created.ErrorCode, len(roomKey), err)
	}
	roomID := func(reader *rmc.Reader) uint32 {
		t.Helper()
		_, room, err := reader.Struct()
		if err != nil {
			t.Fatal(err)
		}
		gid, err := room.U32()
		if err != nil || gid == 0 {
			t.Fatal("invalid gathering ID")
		}
		return gid
	}
	want := roomID(rmc.NewReader(created.Body))
	listed := guest.call(ProtoMatchmakeExt, 0x34, nil)
	reader := rmc.NewReader(listed.Body)
	if count, err := reader.ListHeader(); listed.IsError || err != nil || count != 1 || roomID(reader) != want {
		t.Fatal("guest did not discover the exact created gathering")
	}
	joined := guest.call(ProtoMatchmakeExt, 0x1E, u32le(want))
	key, err := rmc.NewReader(joined.Body).Buffer()
	if joined.IsError || err != nil || !bytes.Equal(key, roomKey) {
		t.Fatal("guest did not join the exact authorized game session")
	}
	playing := guest.call(ProtoMatchmakeExt, 0x1F, nil)
	reader = rmc.NewReader(playing.Body)
	if count, err := reader.ListHeader(); playing.IsError || err != nil || count != 1 {
		t.Fatal("guest lacks the expected room membership")
	}
	_, member, err := reader.Struct()
	if err != nil {
		t.Fatal(err)
	}
	if pid, err := member.U64(); err != nil || pid != guest.pid {
		t.Fatal("room membership did not retain the authenticated identity")
	}
	if id, err := member.U32(); err != nil || id != want {
		t.Fatal("room membership did not retain the joined gathering")
	}
	for _, peer := range []*client{guest, host} {
		if result := peer.call(protocols.ProtoMatchMaking, 0x0C, u32le(want)); result.IsError {
			t.Fatalf("leave result %#x", result.ErrorCode)
		}
	}
	empty := guest.call(ProtoMatchmakeExt, 0x34, nil)
	if count, err := rmc.NewReader(empty.Body).ListHeader(); empty.IsError || err != nil || count != 0 {
		t.Fatal("departed gathering still visible")
	}
}
