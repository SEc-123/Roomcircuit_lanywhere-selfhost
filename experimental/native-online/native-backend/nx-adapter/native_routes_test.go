package main

import (
	"bytes"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"image"
	"image/jpeg"
	"math/big"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"roomcircuit.local/nx-adapter/wire"
)

func runNativeRequest(t *testing.T, handler http.Handler, method, path, mime, payload, token string, want int) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(method, path, strings.NewReader(payload))
	request.Header.Set("Content-Type", mime)
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	result := httptest.NewRecorder()
	handler.ServeHTTP(result, request)
	if result.Code != want {
		t.Fatalf("%s %s status=%d, want %d. Body: %s", method, path, result.Code, want, result.Body.String())
	}
	return result
}

func claimsFromOwnKeySet(t *testing.T, token, setJSON, expectedJKU string) map[string]any {
	t.Helper()
	var set struct{ Keys []map[string]string }
	parts := strings.Split(token, ".")
	if len(parts) != 3 || json.Unmarshal([]byte(setJSON), &set) != nil || len(set.Keys) == 0 {
		t.Fatal("invalid token or public key response")
	}
	var header map[string]string
	metadata, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil || json.Unmarshal(metadata, &header) != nil || header["kid"] == "" ||
		header["alg"] != "RS256" || header["jku"] != expectedJKU {
		t.Fatal("JWT header does not reference its owned key service")
	}
	var matched map[string]string
	for _, key := range set.Keys {
		if key["kid"] == header["kid"] && key["alg"] == "RS256" && key["kty"] == "RSA" {
			matched = key
			break
		}
	}
	if matched == nil {
		t.Fatal("named signing key is absent")
	}
	n, nErr := base64.RawURLEncoding.DecodeString(matched["n"])
	e, eErr := base64.RawURLEncoding.DecodeString(matched["e"])
	signature, sigErr := base64.RawURLEncoding.DecodeString(parts[2])
	hash := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	public := &rsa.PublicKey{N: new(big.Int).SetBytes(n), E: int(new(big.Int).SetBytes(e).Int64())}
	if nErr != nil || eErr != nil || sigErr != nil || rsa.VerifyPKCS1v15(public, crypto.SHA256, hash[:], signature) != nil {
		t.Fatal("could not verify signature against the public key")
	}
	var claims map[string]any
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil || json.Unmarshal(payload, &claims) != nil {
		t.Fatal("invalid signed claims")
	}
	return claims
}

func TestProvisionalProfileKeepsLocalRecordAcrossLink(t *testing.T) {
	const app, user, deviceID = "0000000000001234", "0000000000003456", "000000000000789a"
	draft := player{User: user}
	if baasBindingID(draft) == baasBindingID(player{User: "0000000000004567"}) {
		t.Fatal("draft profiles share a binding identity")
	}
	dir := t.TempDir()
	statePath, stateKey := filepath.Join(dir, "device-state"), filepath.Join(dir, "device-key")
	if err := os.WriteFile(stateKey, []byte(randomHex(32)), 0o600); err != nil {
		t.Fatal(err)
	}
	store, devices, err := openDeviceStore(statePath, stateKey, app)
	if err != nil {
		t.Fatal(err)
	}
	defer store.lock.Close()
	created := time.Now().Unix()
	devices[deviceID] = device{provisional: user, enrollment: deviceKey{1}, createdAt: created, registeredAt: created}
	adapter := &adapter{devices: devices}
	profiles, err := newNativeProfileStore(store)
	if err != nil {
		t.Fatal(err)
	}
	b := &nativeBAAS{adapter: adapter, profiles: profiles, cdn: "https://roomcircuit.example.test"}
	record := nativeProfile{
		CreatedAt: created, UpdatedAt: created,
		Profile: wire.BaaSProfile{Nickname: "Local profile", NicknameUpdatedAt: created},
	}
	apply := func(path, value string) bool {
		return applyProvisionalIdentity(&record, wire.ProfilePatch{Op: "replace", Path: path, Value: value})
	}
	if !apply("/country", "GB") || !apply("/birthday", "2000-02-29") ||
		apply("/country", "gb") || apply("/birthday", "2001-02-29") || apply("/birthday", "2100-01-01") {
		t.Fatal("draft identity accepted invalid metadata or lost valid metadata")
	}
	if err := b.writeOwnedProfile(draft, record); err != nil {
		t.Fatal(err)
	}
	saved, err := b.readOwnedProfile(draft)
	if err != nil {
		t.Fatal(err)
	}
	before, err := wire.BuildBaaSUser(b.snapshot(draft, saved))
	if err != nil || before.Country != "GB" || before.Birthday != "2000-02-29" ||
		before.Nickname != "Local profile" || before.Links.Account != nil || len(before.DeviceAccounts) != 1 {
		t.Fatal("draft profile does not reflect the same local owner")
	}
	linked := player{PID: 1, Account: "verified-owner", User: user, Country: "US", Application: app}
	adapter.mu.Lock()
	devices[deviceID] = device{owner: linked, createdAt: created, linkedAt: created + 1}
	adapter.mu.Unlock()
	if _, err := b.readOwnedProfile(draft); !errors.Is(err, errInactiveProvisionalProfile) {
		t.Fatal("old draft read survived linking")
	}
	if err := b.writeOwnedProfile(draft, record); !errors.Is(err, errInactiveProvisionalProfile) {
		t.Fatal("old draft write survived linking")
	}
	saved, err = b.readOwnedProfile(linked)
	if err != nil {
		t.Fatal(err)
	}
	after, err := wire.BuildBaaSUser(b.snapshot(linked, saved))
	if err != nil || after.ID != before.ID || after.Country != "US" || after.Birthday != before.Birthday ||
		after.Nickname != before.Nickname || after.Links.Account == nil || after.Links.Account.ID != naSubject(linked) {
		t.Fatal("linked profile lost local data or replaced the verified country")
	}
}

func TestMembershipUsesLiveGrantWithRetainedLocalBaaSID(t *testing.T) {
	const app, clientID = "0000000000001234", "0000000000004321"
	owner := player{
		PID: 1, User: "0000000000000011", Account: "owned-profile", Application: app, GrantID: strings.Repeat("a", 48),
	}
	local := owner
	local.User = "0000000000000022"
	authority := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		respond(w, 200, owner)
	}))
	defer authority.Close()
	signingKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	const issuer = "https://accounts.roomcircuit.example.test"
	na, err := newNAIssuer(issuer, "owned-grant-fixture", map[string]*url.URL{clientID: {Scheme: "test", Host: "auth"}}, signingKey)
	if err != nil {
		t.Fatal(err)
	}
	a := &adapter{
		na: na, application: app, grantURL: authority.URL, client: authority.Client(),
		devices: map[string]device{"device": {owner: local, grant: durableGrantPrefix + owner.GrantID}},
	}
	now := time.Now()
	token, err := na.signed(naProof{
		Issuer: issuer, Audience: clientID, Subject: naSubject(owner), Purpose: "token",
		Issued: now.Unix(), Expires: now.Add(time.Minute).Unix(), JWTID: uuid4(), Grant: owner.GrantID,
	})
	if err != nil {
		t.Fatal(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v1/users/{id}/memberships", a.memberships)
	route := "/v1/users/" + naSubject(owner) + "/memberships"
	runNativeRequest(t, mux, "GET", route, "", "", token, 200)
	delete(a.devices, "device")
	runNativeRequest(t, mux, "GET", route, "", "", token, 403)
}

func TestOwnedNativeProfileAndMembership(t *testing.T) {
	t.Setenv("NX_LAB_TITLE_OAUTH_CLIENT_ID", "")
	t.Setenv("NX_LAB_BAAS_CDN_ORIGIN", "")
	const app, user, deviceID = "0000000000001234", "0000000000003456", "000000000000789a"
	owner := player{PID: 3456, User: user, Account: "owner", Nickname: "Player", Country: "US", Application: app, GrantID: strings.Repeat("a", 48)}
	var revoked atomic.Bool
	authority := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if revoked.Load() {
			w.WriteHeader(403)
			return
		}
		var check struct {
			Grant string `json:"grant_id"`
		}
		if r.URL.Path != "/internal/grants/check" || json.NewDecoder(r.Body).Decode(&check) != nil || check.Grant != owner.GrantID {
			w.WriteHeader(401)
			return
		}
		respond(w, 200, owner)
	}))
	defer authority.Close()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	statePath, stateKey := filepath.Join(dir, "device-state"), filepath.Join(dir, "device-key")
	if err := os.WriteFile(stateKey, []byte(randomHex(32)), 0o600); err != nil {
		t.Fatal(err)
	}
	store, devices, err := openDeviceStore(statePath, stateKey, app)
	if err != nil {
		t.Fatal(err)
	}
	defer store.lock.Close()
	grant := durableGrantPrefix + owner.GrantID
	devices[deviceID] = device{owner: owner, grant: grant, createdAt: 100, linkedAt: 120}
	expires, sessionID := time.Now().Add(5*time.Minute), randomHex(24)
	a := &adapter{
		issuer: "https://roomcircuit.example.test", application: app, rsaKey: key,
		grantURL: authority.URL, client: authority.Client(), store: store, devices: devices,
		sessions: map[string]session{sessionID: {deviceID: deviceID, account: owner.Account, grant: grant, validUntil: expires}},
	}
	token, err := a.signed(proof{Issuer: a.issuer, Audience: app, Subject: user, Purpose: "token", Session: sessionID, Expires: expires.Unix()})
	if err != nil {
		t.Fatal(err)
	}
	mux := http.NewServeMux()
	if err := a.addNativeBAASRoutes(mux); err != nil {
		t.Fatal(err)
	}
	own := "/1.0.0/users/" + user
	call := func(method, path, body, bearer string, want int) *httptest.ResponseRecorder {
		t.Helper()
		return runNativeRequest(t, mux, method, path, "application/json", body, bearer, want)
	}
	call("GET", own, "", "", 401)
	call("GET", "/1.0.0/users/ffffffffffffffff", "", token, 403)
	result := call("GET", own, "", token, 200)
	if !strings.Contains(result.Body.String(), naSubject(owner)) || strings.Contains(result.Body.String(), "password") ||
		!strings.Contains(result.Body.String(), `"createdAt":100`) || !strings.Contains(result.Body.String(), `"createdAt":120`) {
		t.Fatal("linked profile did not preserve identity and device privacy")
	}
	call("PATCH", own, `[{"op":"replace","path":"/nickname","value":"Room Name"},{"op":"replace","path":"/id","value":"bad"}]`, token, 400)
	if strings.Contains(call("GET", own, "", token, 200).Body.String(), "Room Name") {
		t.Fatal("invalid patch wrote a partial update")
	}
	call("PATCH", own, `[{"op":"replace","path":"/nickname","value":"Room Name"}]`, token, 200)
	if !strings.Contains(call("GET", own, "", token, 200).Body.String(), "Room Name") {
		t.Fatal("own profile update did not persist")
	}
	var jpegData bytes.Buffer
	if err := jpeg.Encode(&jpegData, image.NewGray(image.Rect(0, 0, 8, 8)), nil); err != nil {
		t.Fatal(err)
	}
	jpegData.WriteString("trailing-private-marker")
	raw, _ := json.Marshal(map[string]string{"rawContent": base64.StdEncoding.EncodeToString(jpegData.Bytes())})
	call("POST", "/1.0.0/image_upload", "{}", token, 400)
	result = call("POST", "/1.0.0/image_upload", string(raw), token, 200)
	var avatar struct {
		Content struct{ URL, Format string }
	}
	if json.Unmarshal(result.Body.Bytes(), &avatar) != nil || avatar.Content.Format != "jpg" {
		t.Fatal("wrong uploaded avatar reply")
	}
	avatarURL, err := url.Parse(avatar.Content.URL)
	if err != nil || avatarURL.Host != "roomcircuit.example.test" || avatarURL.Path != "/1/"+user {
		t.Fatal("avatar reply does not name the owner's service")
	}
	confirmation, _ := json.Marshal([]map[string]string{{"op": "replace", "path": "/thumbnailUrl", "value": avatar.Content.URL}})
	call("PATCH", own, string(confirmation), token, 200)
	call("PATCH", own, `[{"op":"replace","path":"/thumbnailUrl","value":"https://other.example.test/avatar.jpg"}]`, token, 400)
	call("GET", avatarURL.RequestURI(), "", "", 401)
	result = call("GET", avatarURL.RequestURI(), "", token, 200)
	if bytes.Contains(result.Body.Bytes(), []byte("trailing-private-marker")) || result.Header().Get("Content-Type") != "image/jpeg" {
		t.Fatal("avatar content was not bounded and normalized")
	}
	encoded, err := os.ReadFile(filepath.Join(a.baas.profiles.dir, user))
	if err != nil || bytes.Contains(encoded, []byte("Room Name")) {
		t.Fatal("owned profile persisted in plaintext")
	}
	collection := "/v1/users/" + naSubject(owner) + "/memberships"
	call("GET", "/v1/users/"+user+"/memberships", "", token, 403)
	result = call("GET", collection, "", token, 200)
	var membership struct {
		Count, Total int
		Items        []struct {
			Context    string
			Membership struct{ Active bool }
		}
	}
	if json.Unmarshal(result.Body.Bytes(), &membership) != nil || membership.Count != 2 || membership.Total != 2 ||
		len(membership.Items) != 2 || membership.Items[0].Context != "default" || !membership.Items[0].Membership.Active ||
		membership.Items[1].Context != "ex" || membership.Items[1].Membership.Active {
		t.Fatal("membership did not reflect independent realm grant")
	}
	revoked.Store(true)
	call("GET", collection, "", token, 403)
	call("PATCH", own, `[{"op":"replace","path":"/nickname","value":"revoked"}]`, token, 403)
}

func TestNativeApplicationAuthorization(t *testing.T) {
	const app, clientID = "0000000000001234", "000000000000f1a0"
	redirect, err := url.Parse("npf" + clientID + "://auth")
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("NX_LAB_TITLE_OAUTH_CLIENT_ID", clientID)
	t.Setenv("NX_LAB_AAUTH_KEY_ORIGIN", "")
	t.Setenv("NX_LAB_AAUTH_ORIGIN", "")
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	deviceAuth := randomHex(16)
	binding := deviceKey{1}
	a := &adapter{issuer: "https://roomcircuit.example.test", application: app, rsaKey: key}
	a.na = &naIssuer{audiences: map[string]*url.URL{clientID: redirect}}
	a.bootstrap = &deviceBootstrap{
		clientID: hos19GameDevice,
		assertions: map[[32]byte]deviceProof{sha256.Sum256([]byte(deviceAuth)): {
			key: binding, clientID: hos19GameDevice, expires: time.Now().Add(time.Minute),
		}},
	}
	mux := http.NewServeMux()
	if err := a.addNativeAAUTH(mux); err != nil {
		t.Fatal(err)
	}
	const route, form = "/v5/application_auth_token", "application/x-www-form-urlencoded"
	args := url.Values{"application_id": {app}, "application_version": {"00000001"}, "device_auth_token": {"unknown"}}
	invalid := runNativeRequest(t, mux, "POST", route, form, args.Encode(), "", 401)
	verifyNDASError(t, invalid.Body.String(), "0118")
	args.Set("device_auth_token", deviceAuth)
	args.Set("application_id", "000000000000abcd")
	wrongTitle := runNativeRequest(t, mux, "POST", route, form, args.Encode(), "", 403)
	verifyNDASError(t, wrongTitle.Body.String(), "0106")
	args.Set("application_id", app)
	r := runNativeRequest(t, mux, "POST", route, form, args.Encode(), "", 200)
	var application struct {
		Token    string `json:"application_auth_token"`
		Settings []map[string]string
	}
	if json.Unmarshal(r.Body.Bytes(), &application) != nil || len(application.Settings) != 1 ||
		application.Settings[0]["nas:client_id"] != clientID || application.Settings[0]["nas:redirect_uri"] != redirect.String() {
		t.Fatal("title authorization registry and callback do not match")
	}
	if !a.verifyNativeAppToken(application.Token, binding) || a.verifyNativeAppToken(application.Token, deviceKey{2}) ||
		a.verifyNativeAppToken(application.Token+"changed", binding) {
		t.Fatal("application token was not integrity and enrollment bound")
	}
	keys := runNativeRequest(t, mux, "GET", "/keys", "", "", "", 200)
	claims := claimsFromOwnKeySet(t, application.Token, keys.Body.String(), a.issuer+"/keys")
	device, ok := claims["nintendo"].(map[string]any)
	edi, _ := device["edi"].(string)
	if claims["typ"] != "application_auth_token" || claims["iss"] != "roomcircuit.example.test" ||
		claims["aud"] != clientID || claims["sub"] != app || !ok || len(edi) != 32 {
		t.Fatal("AAUTH proof does not identify its owned title and client")
	}
}
