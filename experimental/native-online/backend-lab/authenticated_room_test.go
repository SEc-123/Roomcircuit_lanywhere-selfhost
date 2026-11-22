// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"fmt"
	"math/big"
	"net"
	"net/url"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/openpak/switch-nex/protocols"
	"github.com/openpak/switch-nex/prudplite"
	"github.com/openpak/switch-nex/rmc"
)

func ownTLS(t *testing.T) (server, client *tls.Config) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	certificate := &x509.Certificate{
		SerialNumber: big.NewInt(1), NotBefore: now.Add(-time.Minute), NotAfter: now.Add(time.Hour),
		IPAddresses: []net.IP{net.IPv4(127, 0, 0, 1)},
		KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IsCA:        true, BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, certificate, certificate, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	trusted, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	roots := x509.NewCertPool()
	roots.AddCert(trusted)
	return &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}}},
		&tls.Config{MinVersion: tls.VersionTLS12, RootCAs: roots}
}

func labListeners(t *testing.T) (net.Listener, net.Listener, int) {
	t.Helper()
	for attempt := 0; attempt < 8; attempt++ {
		auth, err := net.Listen("tcp4", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		base := auth.Addr().(*net.TCPAddr).Port
		secure, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", base+1))
		if err == nil {
			return auth, secure, base
		}
		auth.Close()
	}
	t.Fatal("cannot reserve adjacent loopback ports")
	return nil, nil, 0
}

func ownSecret(t *testing.T, length int) []byte {
	t.Helper()
	value := make([]byte, length)
	if _, err := rand.Read(value); err != nil {
		t.Fatal(err)
	}
	return value
}

func TestRoomCircuitAuthenticatedRoom(t *testing.T) {
	for _, profile := range []string{"pico-park", "mario-party-superstars"} {
		t.Run(profile, func(t *testing.T) { testAuthenticatedRoom(t, profile) })
	}
}

func testAuthenticatedRoom(t *testing.T, titleProfile string) {
	if titleProfile == "mario-party-superstars" {
		t.Setenv("MPS_INTEGER_SETTINGS", "")
	}
	identityURL, err := url.Parse(os.Getenv("RC_IDENTITY_URL"))
	if err != nil || identityURL == nil || identityURL.Scheme != "http" || identityURL.Hostname() != "127.0.0.1" ||
		identityURL.User != nil || identityURL.Path != "" || identityURL.RawQuery != "" || identityURL.Fragment != "" {
		t.Fatal("RC_IDENTITY_URL must be a loopback HTTP origin")
	}
	internalKey := os.Getenv("RC_IDENTITY_KEY")
	if internalKey == "" {
		t.Fatal("missing private game-to-account service credential")
	}
	expectedPID := func(variable string) uint64 {
		value, err := strconv.ParseUint(os.Getenv(variable), 10, 64)
		if err != nil || value == 0 {
			t.Fatalf("missing stable game principal in %s", variable)
		}
		return value
	}
	hostPID, guestPID := expectedPID("RC_HOST_PID"), expectedPID("RC_GUEST_PID")
	hostToken, guestToken := os.Getenv("RC_HOST_TOKEN"), os.Getenv("RC_GUEST_TOKEN")
	if hostPID == guestPID || hostToken == "" || guestToken == "" || hostToken == guestToken {
		t.Fatal("two distinct independent account identities are required")
	}
	wasRequired := *requireIdentity
	*requireIdentity = true
	t.Cleanup(func() { *requireIdentity = wasRequired })
	ids := newIdentity(identityURL.String(), internalKey)
	if !ids.enabled() {
		t.Fatal("account identity enforcement must be enabled")
	}
	// The lab owns this realm, its transport credential and its ticket secret.
	// Copy only the title's public matchmaking behavior into that realm.
	var configure func(*protocols.Env)
	for _, profile := range titles {
		if profile.name == titleProfile {
			configure = profile.env
			break
		}
	}
	if configure == nil {
		t.Fatal("published matchmaking profile missing from the pinned source")
	}
	authListener, secureListener, base := labListeners(t)
	serverTLS, clientTLS := ownTLS(t)
	accessKey := hex.EncodeToString(ownSecret(t, 4))
	ticketKey := ownSecret(t, 16)
	realm := "roomcircuit-lab/" + titleProfile
	env := protocols.NewEnv(realm, "127.0.0.1", base)
	configure(env)
	recall := newRecall(time.Hour, "")
	svc := &service{env: env, base: base, ids: ids, targetKey: ticketKey}
	svc.auth = &prudplite.Endpoint{Name: realm + "/auth", StreamID: 1, AccessKey: accessKey,
		OnRMC: func(connection *prudplite.Connection, request *rmc.RMCMessage) []byte {
			return handleAuthRMC(ids, recall, env, ticketKey, connection, request)
		}}
	svc.secure = &prudplite.Endpoint{Name: realm + "/secure", StreamID: 1, AccessKey: accessKey,
		IsSecure: true, KerberosKey: ticketKey,
		OnRMC: func(connection *prudplite.Connection, request *rmc.RMCMessage) []byte {
			return protocols.Dispatch(env, connection, request)
		}, OnDisconnect: func(connection *prudplite.Connection) { env.Disconnected(connection.PID) }}
	env.BindPush(svc.secure)
	svc.serve(tls.NewListener(authListener, serverTLS), tls.NewListener(secureListener, serverTLS))
	t.Cleanup(func() { svc.auth.Close(); svc.secure.Close() })

	dialVerified := func(listener net.Listener) *client {
		connection, err := tls.DialWithDialer(&net.Dialer{Timeout: 5 * time.Second}, "tcp", listener.Addr().String(), clientTLS)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { connection.Close() })
		return &client{t: t, conn: connection, accessKey: accessKey}
	}
	login := func(token string) (*client, *rmc.RMCMessage) {
		auth := dialVerified(authListener)
		auth.handshake(nil)
		request := rmc.NewWriter()
		request.String("Shared display name")
		request.Raw(authInfoHolder(token))
		return auth, auth.call(0x0A, 0x02, request.Bytes())
	}
	if _, denied := login(hex.EncodeToString(ownSecret(t, 32))); !denied.IsError {
		t.Fatal("invalid credential received a game ticket")
	}
	openSession := func(token string, wantPID uint64) *client {
		auth, result := login(token)
		if result.IsError {
			t.Fatalf("account-backed login rejected: result %#x", result.ErrorCode)
		}
		auth.readLoginExResult(result.Body)
		auth.openTicket()
		if auth.pid != wantPID || len(auth.sessionKey) == 0 {
			t.Fatal("authenticated ticket does not identify its independent account")
		}
		connection := dialVerified(secureListener)
		connection.pid, connection.ticket, connection.sessionKey = auth.pid, auth.ticket, auth.sessionKey
		connection.handshake(connection.secureConnectPayload(1))
		return connection
	}
	host, guest := openSession(hostToken, hostPID), openSession(guestToken, guestPID)
	if _, ok := svc.secure.ConnectionByPID(hostPID); !ok {
		t.Fatal("host principal absent from the secure endpoint")
	}
	if _, ok := svc.secure.ConnectionByPID(guestPID); !ok {
		t.Fatal("guest principal absent from the secure endpoint")
	}
	if titleProfile == "mario-party-superstars" {
		auditMarioPartySettings(t, host)
	}
	st := rmc.NewWriter()
	st.List(1)
	st.String("prudp:/address=127.0.0.1;port=50000;sid=1;natf=0")
	if result := host.call(ProtoSecureConnection, 0x01, st.Bytes()); result.IsError {
		t.Fatalf("host station registration failed: result %#x", result.ErrorCode)
	}
	created := host.call(ProtoMatchmakeExt, 0x06, sessionHolderBytes(2, 3, 77))
	if created.IsError {
		t.Fatalf("create room failed: result %#x", created.ErrorCode)
	}
	participants, key, err := readJoinedSession(created.Body)
	if err != nil || (env.FinalizeCreateSession && (participants != 1 || len(key) != 32)) {
		t.Fatalf("create returned count %d and key length %d: %v", participants, len(key), err)
	}
	_, createdRoom, err := rmc.NewReader(created.Body).Struct()
	if err != nil {
		t.Fatal(err)
	}
	createdID, err := createdRoom.U32()
	if err != nil || createdID == 0 {
		t.Fatal("invalid created room identity")
	}
	listed := guest.call(ProtoMatchmakeExt, 0x34, nil)
	if listed.IsError {
		t.Fatalf("list rooms failed: result %#x", listed.ErrorCode)
	}
	rooms := rmc.NewReader(listed.Body)
	if count, err := rooms.ListHeader(); err != nil || count != 1 {
		t.Fatalf("expected one exact room, got %d: %v", count, err)
	}
	_, room, err := rooms.Struct()
	if err != nil {
		t.Fatal(err)
	}
	roomID, err := room.U32()
	if err != nil || roomID != createdID {
		t.Fatal("listed room does not match the created room")
	}
	joined := guest.call(ProtoMatchmakeExt, 0x1E, u32le(roomID))
	if joined.IsError {
		t.Fatalf("join room failed: result %#x", joined.ErrorCode)
	}
	returnedKey, err := rmc.NewReader(joined.Body).Buffer()
	if err != nil || !bytes.Equal(returnedKey, key) {
		t.Fatal("joined room does not match the host's session")
	}
	playing := guest.call(ProtoMatchmakeExt, 0x1F, nil)
	view := rmc.NewReader(playing.Body)
	if count, err := view.ListHeader(); playing.IsError || err != nil || count != 1 {
		t.Fatal("guest lacks exactly one joined session")
	}
	_, membership, err := view.Struct()
	if err != nil {
		t.Fatal(err)
	}
	if observed, err := membership.U64(); err != nil || observed != guestPID {
		t.Fatal("room membership does not identify the authenticated guest")
	}
	if observed, err := membership.U32(); err != nil || observed != createdID {
		t.Fatal("guest was seated in a different room")
	}
	for _, peer := range []*client{guest, host} {
		if result := peer.call(protocols.ProtoMatchMaking, 0x0C, u32le(roomID)); result.IsError {
			t.Fatalf("room leave failed: result %#x", result.ErrorCode)
		}
	}
	empty := guest.call(ProtoMatchmakeExt, 0x34, nil)
	if count, err := rmc.NewReader(empty.Body).ListHeader(); empty.IsError || err != nil || count != 0 {
		t.Fatal("room is still listed after both authenticated participants left")
	}
	t.Log("Account-backed game login, credential rejection, TLS, secure tickets, exact room create/list/join and cleanup passed")
}

func auditMarioPartySettings(t *testing.T, host *client) {
	t.Helper()
	result := host.call(ProtoUtility, 0x07, u32le(0))
	r := rmc.NewReader(result.Body)
	count, err := r.ListHeader()
	want := map[uint16]uint32{0: 1000, 1: 1000, 2: 500, 3: 250, 4: 20, 5: 1}
	if result.IsError || err != nil || count != uint32(len(want)) {
		t.Fatal("Mario Party bootstrap did not return its six title settings")
	}
	for range count {
		key, keyErr := r.U16()
		value, valueErr := r.U32()
		if expected, found := want[key]; keyErr != nil || valueErr != nil || !found || value != expected {
			t.Fatal("Mario Party bootstrap returned an invalid title setting")
		}
		delete(want, key)
	}
}
