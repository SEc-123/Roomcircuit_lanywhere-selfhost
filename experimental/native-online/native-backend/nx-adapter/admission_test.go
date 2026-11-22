package main

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestGameRevalidationOutlivesLoginButHonorsRevocation(t *testing.T) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	var upstream atomic.Int32
	upstream.Store(200)
	playerFixture := player{PID: 42, User: "000000000000002a", Account: "fixture", Application: "01006fe013472000", GrantID: strings.Repeat("a", 48)}
	broker := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var input map[string]string
		if r.URL.Path != "/internal/grants/check" || r.Header.Get("X-Internal-Key") != "private-fixture" || json.NewDecoder(r.Body).Decode(&input) != nil ||
			input["grant_id"] != playerFixture.GrantID || input["application_id"] != playerFixture.Application {
			t.Error("game did not use the verified stored title grant")
			w.WriteHeader(500)
			return
		}
		respond(w, int(upstream.Load()), playerFixture)
	}))
	defer broker.Close()
	saved := durableGrantPrefix + playerFixture.GrantID
	a := &adapter{
		application: playerFixture.Application, issuer: "https://own-realm.example", rsaKey: key,
		grantURL: broker.URL, grantKey: "private-fixture", client: broker.Client(),
		devices:  map[string]device{"device": {owner: playerFixture, grant: saved}},
		sessions: map[string]session{"session": {deviceID: "device", account: playerFixture.Account, grant: saved, validUntil: time.Now().Add(time.Minute)}},
		admitted: make(map[uint64]gameAdmission),
	}
	ctx := context.Background()
	if _, status := a.connectedPlayer(ctx, playerFixture.PID); status != 404 {
		t.Fatalf("bare PID auto-admitted: %d", status)
	}
	c := proof{Issuer: a.issuer, Audience: a.application, Subject: playerFixture.User, Purpose: "id_token", Session: "session", Expires: time.Now().Add(time.Minute).Unix()}
	jwt, err := a.signed(c)
	if err != nil {
		t.Fatal(err)
	}
	if _, status := a.authenticated(ctx, jwt, "id_token"); status != 403 {
		t.Fatalf("account-only login incorrectly allowed title admission: %d", status)
	}
	login := a.sessions["session"]
	login.forGame = true
	a.sessions["session"] = login
	if _, status := a.authenticated(ctx, jwt, "id_token"); status != 200 {
		t.Fatalf("valid game ID not admitted: %d", status)
	}
	delete(a.sessions, "session")
	c.Expires = time.Now().Add(-time.Second).Unix()
	expired, err := a.signed(c)
	if err != nil {
		t.Fatal(err)
	}
	if _, status := a.authenticated(ctx, expired, "id_token"); status != 401 {
		t.Fatalf("public expired game token accepted: %d", status)
	}
	if who, status := a.connectedPlayer(ctx, playerFixture.PID); status != 200 || who.PID != playerFixture.PID {
		t.Fatalf("authorized live game disconnected after login expiry: %d", status)
	}
	upstream.Store(503)
	if _, status := a.connectedPlayer(ctx, playerFixture.PID); status != 503 {
		t.Fatalf("transient failure should retain its status: %d", status)
	}
	upstream.Store(404)
	if _, status := a.connectedPlayer(ctx, playerFixture.PID); status != 403 {
		t.Fatalf("definitively revoked active game should be rejected: %d", status)
	}
}
