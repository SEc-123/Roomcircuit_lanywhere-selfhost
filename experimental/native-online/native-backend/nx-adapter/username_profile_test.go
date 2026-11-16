package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/rsa"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"roomcircuit.local/nx-adapter/wire"
)

func TestUsernameOAuthFormAndAccountLogin(t *testing.T) {
	var body bytes.Buffer
	if err := linkForm.Execute(&body, map[string]any{"Token": "example", "UsernameRealm": true, "RegistrationURL": "https://accounts.own.test/register"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(body.String(), `name="username"`) || strings.Contains(body.String(), `name="email"`) || !strings.Contains(body.String(), `href="https://accounts.own.test/register"`) {
		t.Fatal("username realm presented an email form")
	}
	seen := make(chan map[string]string, 1)
	account := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var got map[string]string
		if json.NewDecoder(r.Body).Decode(&got) != nil {
			t.Error("invalid login request")
		}
		seen <- got
		w.WriteHeader(http.StatusUnauthorized)
	}))
	defer account.Close()
	flow := &linkOAuth{a: &adapter{client: account.Client()}, account: account.URL, usernameRealm: true}
	if _, status := flow.brokerApproval(context.Background(), "player_one", "not-a-real-password"); status != 401 {
		t.Fatalf("expected failed account login, got %d", status)
	}
	got := <-seen
	if got["username"] != "player_one" || got["email"] != "" || got["password"] != "not-a-real-password" {
		t.Fatal("username login did not use the username contract")
	}
}

func TestUsernameOAuthTemporarySessionClosedAcrossRepeatedFailures(t *testing.T) {
	created, closed := 0, 0
	account := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/login":
			created++
			http.SetCookie(w, &http.Cookie{Name: "openpak_session", Value: "temporary-session"})
			respond(w, 200, map[string]string{"account_id": "own-account"})
		case "/v1/logout":
			cookie, err := r.Cookie("openpak_session")
			if err != nil || cookie.Value != "temporary-session" {
				t.Error("logout did not use the issued session")
			}
			closed++
			w.WriteHeader(204)
		default:
			w.WriteHeader(404)
		}
	}))
	defer account.Close()
	broker := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(503) }))
	defer broker.Close()
	flow := &linkOAuth{a: &adapter{client: account.Client(), na: &naIssuer{origin: "https://account.own.test"}},
		browser: broker.URL, account: account.URL, client: broker.Client(), usernameRealm: true}
	for i := 0; i < 25; i++ {
		if _, status := flow.brokerApproval(context.Background(), "player", "test-password"); status != 503 {
			t.Fatalf("attempt %d: expected unavailable broker, got %d", i, status)
		}
	}
	if created != 25 || closed != created {
		t.Fatalf("temporary sessions created=%d closed=%d", created, closed)
	}
}

func TestUsernameOAuthInternalBrokerAndSourceLimits(t *testing.T) {
	const privateKey = "local-internal-test-key"
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Internal-Key") != privateKey {
			w.WriteHeader(401)
			return
		}
		w.WriteHeader(204)
	}))
	defer server.Close()
	flow := &linkOAuth{a: &adapter{grantKey: privateKey}, browser: server.URL, client: server.Client(), usernameRealm: true}
	response, err := flow.call(context.Background(), "/device", "", "", nil)
	if err != nil || response.StatusCode != 204 {
		t.Fatalf("private broker request failed: %v", err)
	}
	response.Body.Close()
	r := httptest.NewRequest("POST", "/connect/1.0.0/authorize", nil)
	r.RemoteAddr = "192.0.2.1:3210"
	for i := 0; i < 20; i++ {
		r.Header.Set("X-Forwarded-For", "192.0.2."+string(rune('a'+i)))
		if !flow.allowBrowser(r, "approve", 20) {
			t.Fatalf("rejected valid attempt %d", i)
		}
	}
	r.Header.Set("X-Forwarded-For", "203.0.113.25")
	if flow.allowBrowser(r, "approve", 20) {
		t.Fatal("spoofed forwarded address bypassed source limit")
	}
	r.RemoteAddr = "192.0.2.2:3210"
	if !flow.allowBrowser(r, "approve", 20) {
		t.Fatal("a different socket peer inherited another source limit")
	}
}

func TestUsernameGrantBuildsLinkedGameProfileWithoutCountry(t *testing.T) {
	const app, user, deviceID = "0000000000001234", "0000000000003456", "000000000000789a"
	const grant = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	broker := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		respond(w, 200, map[string]any{"pid": 1, "baas_user_id": user, "account_id": "own-user", "account_type": "username",
			"application_id": app, "nickname": "player_one", "country": "", "grant_id": grant})
	}))
	defer broker.Close()
	a := &adapter{issuer: "https://baas.own.test", application: app, grantURL: broker.URL, grantKey: "local-key", client: broker.Client()}
	who, status := a.revalidateGrant(context.Background(), durableGrantPrefix+grant)
	if status != 200 || who.AccountType != "username" {
		t.Fatalf("username identity: HTTP %d", status)
	}
	input, err := a.userInput(who, deviceID, 100, 101, true)
	if err != nil {
		t.Fatal(err)
	}
	result, err := wire.BuildBaaSLogin(input, wire.BaaSLoginCredentials{IDToken: "own-id", AccessToken: "own-access", ExpiresIn: 300})
	if err != nil || result.User.Country != "" {
		t.Fatalf("linked username profile: %v", err)
	}
}

func TestOwnedUsernameProfileThroughPrivateBroker(t *testing.T) {
	const app, client, grant, internalKey = "0000000000001234", "6ffd70c434d303c8", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "local-test-broker-key"
	who := player{PID: 42, User: "000000000000002a", Account: "own-account", Application: app, GrantID: grant}
	profileType := "username"
	broker := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Internal-Key") != internalKey {
			w.WriteHeader(401)
			return
		}
		switch r.URL.Path {
		case "/internal/grants/check":
			respond(w, 200, who)
		case "/internal/account/profile":
			respond(w, 200, map[string]any{"id": who.User, "nickname": "player_one", "account_type": profileType,
				"email": "", "emailVerified": false, "country": "", "birthday": ""})
		default:
			w.WriteHeader(404)
		}
	}))
	defer broker.Close()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	redirect, _ := url.Parse("nintendo://account.nx.sys")
	issuer, err := newNAIssuer("https://account.own.test", "private-local-test-key", map[string]*url.URL{client: redirect}, key)
	if err != nil {
		t.Fatal(err)
	}
	adapter := &adapter{application: app, grantURL: broker.URL, grantKey: internalKey, client: broker.Client(), na: issuer}
	now := time.Now().Unix()
	token, err := issuer.signed(naProof{Issuer: issuer.origin, Audience: client, Subject: naSubject(who), Issued: now,
		Expires: now + 60, JWTID: "local-test", Purpose: "token", Grant: grant, Scopes: []string{"nx"}})
	if err != nil {
		t.Fatal(err)
	}
	request := func() *httptest.ResponseRecorder {
		r := httptest.NewRequest("GET", "/2.0.0/users/me", nil)
		r.Header.Set("Authorization", "Bearer "+token)
		w := httptest.NewRecorder()
		adapter.ownProfile(w, r)
		return w
	}
	response := request()
	var result map[string]any
	if response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &result) != nil {
		t.Fatalf("username profile HTTP %d", response.Code)
	}
	if result["email"] != "" || result["emailVerified"] != false || result["birthday"] != "" || result["country"] != "" {
		t.Fatal("username profile invented verified contact or personal data")
	}
	profileType = "untrusted-policy"
	if response = request(); response.Code != 503 {
		t.Fatalf("unknown realm type HTTP %d", response.Code)
	}
	profileType = "email"
	if response = request(); response.Code != 409 {
		t.Fatalf("unverified email profile HTTP %d", response.Code)
	}
}
