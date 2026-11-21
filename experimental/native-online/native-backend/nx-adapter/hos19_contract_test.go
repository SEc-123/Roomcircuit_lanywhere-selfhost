// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"maps"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"slices"
	"strings"
	"testing"
	"time"
)

type hos19AccountContract struct {
	Firmware string `json:"firmware"`
	BAAS     struct {
		AnonymousPath         string   `json:"anonymous_path"`
		AnonymousResponseKeys []string `json:"anonymous_response_keys"`
		OptionalPenneField    string   `json:"optional_penne_field"`
	} `json:"baas"`
	OAuth struct {
		AuthorizePath  string   `json:"authorize_path"`
		AuthorizeScope string   `json:"authorize_scope"`
		TokenPath      string   `json:"token_path"`
		ProfileScopes  []string `json:"profile_scopes"`
	} `json:"oauth"`
}

func loadHOS19AccountContract(t *testing.T) hos19AccountContract {
	t.Helper()
	data, err := os.ReadFile("testdata/hos19-account-contract.json")
	if err != nil {
		t.Fatal(err)
	}
	var contract hos19AccountContract
	if err := json.Unmarshal(data, &contract); err != nil || contract.Firmware != "19.0.1" {
		t.Fatalf("could not load the HOS 19 profile: %v", err)
	}
	return contract
}

func hos19Form(handler http.Handler, route string, fields url.Values) *httptest.ResponseRecorder {
	request := httptest.NewRequest(http.MethodPost, route, strings.NewReader(fields.Encode()))
	request.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	return recorder
}

func TestHOS19AnonymousBaaSResponse(t *testing.T) {
	contract := loadHOS19AccountContract(t).BAAS
	t.Setenv("NX_LAB_BAAS_KEY_ORIGIN", "")
	t.Setenv("NX_LAB_BAAS_AUDIENCE", "00aabbccddee1234")
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	const localAssertion = "locally-enrolled-device-assertion"
	const deviceClient = "00aabbccddee1234"
	adapter := &adapter{issuer: "https://account.roomcircuit.example.test", rsaKey: key}
	certificates := http.NewServeMux()
	if err := adapter.addBAASCertificates(certificates); err != nil {
		t.Fatal(err)
	}
	b := &deviceBootstrap{
		adapter: adapter, clientID: deviceClient,
		assertions: map[[32]byte]deviceProof{sha256.Sum256([]byte(localAssertion)): {
			key: deviceKey{1}, clientID: deviceClient, expires: time.Now().Add(time.Minute),
		}},
		tokens: make(map[[32]byte]deviceProof),
	}
	handler := http.HandlerFunc(b.applicationToken)
	form := url.Values{"grantType": {"public_client"}, "assertion": {"not-an-enrolled-device"}}
	if response := hos19Form(handler, contract.AnonymousPath, form); response.Code != http.StatusUnauthorized {
		t.Fatalf("unknown device assertion received HTTP %d", response.Code)
	}
	form.Set("assertion", localAssertion)
	for _, includePenne := range []bool{false, true} {
		if includePenne {
			form.Set(contract.OptionalPenneField, "local-optional-notification-id")
		}
		response := hos19Form(handler, contract.AnonymousPath, form)
		var body map[string]json.RawMessage
		if response.Code != http.StatusOK || json.Unmarshal(response.Body.Bytes(), &body) != nil {
			t.Fatalf("anonymous token exchange returned HTTP %d", response.Code)
		}
		if !slices.Equal(slices.Sorted(maps.Keys(body)), slices.Sorted(slices.Values(contract.AnonymousResponseKeys))) {
			t.Fatal("anonymous token response does not match the HOS 19 shape")
		}
		var access string
		if json.Unmarshal(body["accessToken"], &access) != nil || access == "" {
			t.Fatal("anonymous token response is missing its credential")
		}
		header := hos19JWTPart(t, access, 0)
		claims := hos19JWTPart(t, access, 1)
		kid, _ := header["kid"].(string)
		if header["jku"] != adapter.issuer+"/1.0.0/internal_certificates" || kid == "" ||
			claims["typ"] != "token" || claims["bs:grt"] != float64(1) {
			t.Fatal("anonymous token does not carry the native access role and its own key reference")
		}
		reply := httptest.NewRecorder()
		certificates.ServeHTTP(reply, httptest.NewRequest(http.MethodGet, "/1.0.0/internal_certificates", nil))
		var keySet struct {
			Keys []struct{ Kid, N string }
		}
		if reply.Code != http.StatusOK || json.Unmarshal(reply.Body.Bytes(), &keySet) != nil || len(keySet.Keys) != 1 ||
			keySet.Keys[0].Kid != kid || keySet.Keys[0].N != base64.RawURLEncoding.EncodeToString(key.N.Bytes()) {
			t.Fatal("anonymous token references an unavailable signing key")
		}
		segments := strings.Split(access, ".")
		digest := sha256.Sum256([]byte(segments[0] + "." + segments[1]))
		signature, err := base64.RawURLEncoding.DecodeString(segments[2])
		if err != nil || rsa.VerifyPKCS1v15(&key.PublicKey, crypto.SHA256, digest[:], signature) != nil {
			t.Fatal("anonymous token signature failed against the published key")
		}
	}
	form.Set("grantType", "unknown_grant_type")
	if response := hos19Form(handler, contract.AnonymousPath, form); response.Code != http.StatusBadRequest {
		t.Fatalf("unknown anonymous grant type returned HTTP %d", response.Code)
	}
}

func hos19JWTPart(t *testing.T, token string, part int) map[string]any {
	t.Helper()
	segments := strings.Split(token, ".")
	if len(segments) != 3 {
		t.Fatal("invalid compact token")
	}
	decoded, err := base64.RawURLEncoding.DecodeString(segments[part])
	var object map[string]any
	if err != nil || json.Unmarshal(decoded, &object) != nil {
		t.Fatal("invalid native token metadata")
	}
	return object
}

func TestHOS19OAuthProfileScopeTransition(t *testing.T) {
	contract := loadHOS19AccountContract(t).OAuth
	const app, clientID = "0000000000001234", "6ffd70c434d303c8"
	const issuerURL, brokerToken = "https://account.roomcircuit.example.test", "owned-broker-id-token"
	const verifier = "locally-owned-protocol-test-pkce-verifier-0123456789"
	digest := sha256.Sum256([]byte(verifier))
	challenge := base64.RawURLEncoding.EncodeToString(digest[:])
	player := player{PID: 31, User: "0000000000001235", Account: "local-owned-account", Application: app,
		GrantID: strings.Repeat("a", 48), Country: "IE", Nickname: "Local player"}
	authority := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var claims map[string]string
		if json.NewDecoder(r.Body).Decode(&claims) != nil || claims["application_id"] != app {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		validLogin := r.URL.Path == "/internal/switch/identity" && claims["id_token"] == brokerToken
		validGrant := r.URL.Path == "/internal/grants/check" && claims["grant_id"] == player.GrantID
		if !validLogin && !validGrant {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		respond(w, 200, player)
	}))
	defer authority.Close()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	const redirect = "nintendo://account.nx.sys"
	destination, err := url.Parse(redirect)
	if err != nil {
		t.Fatal(err)
	}
	issuer, err := newNAIssuer(issuerURL, "local-profile-wire-fixture-key", map[string]*url.URL{clientID: destination}, key)
	if err != nil {
		t.Fatal(err)
	}
	flow := &linkOAuth{
		a:     &adapter{issuer: issuerURL, application: app, grantURL: authority.URL, client: authority.Client(), na: issuer},
		flows: make(map[string]oauthFlow), codes: make(map[[32]byte]oauthCode),
	}
	const proofA, proofB, gameProof = "local-account-device-a", "local-account-device-b", "local-game-only-device"
	deviceA, deviceB := deviceKey{1}, deviceKey{2}
	proofUntil := time.Now().Add(time.Minute)
	flow.a.bootstrap = &deviceBootstrap{assertions: map[[32]byte]deviceProof{
		sha256.Sum256([]byte(proofA)):    {key: deviceA, clientID: hos19AccountApplet, expires: proofUntil},
		sha256.Sum256([]byte(proofB)):    {key: deviceB, clientID: hos19AccountAPI, expires: proofUntil},
		sha256.Sum256([]byte(gameProof)): {key: deviceA, clientID: hos19GameDevice, expires: proofUntil},
	}}
	authorize := func(deviceAssertion string) string {
		t.Helper()
		query := url.Values{"scope": {contract.AuthorizeScope}, "client_id": {clientID}, "redirect_uri": {redirect},
			"session_token_code_challenge": {challenge}, "session_token_code_challenge_method": {"S256"},
			"response_type": {"session_token_code"}, "state": {"local-profile-binding"}}
		recorder := httptest.NewRecorder()
		flow.begin(recorder, httptest.NewRequest(http.MethodGet, contract.AuthorizePath+"?"+query.Encode(), nil))
		cookies := recorder.Result().Cookies()
		if recorder.Code != http.StatusOK || len(cookies) != 1 {
			t.Fatalf("native authorize returned HTTP %d", recorder.Code)
		}
		approved := flow.flows[cookies[0].Value]
		delete(flow.flows, cookies[0].Value)
		code := randomHex(24)
		// The fixture supplies an own browser grant. Exercise the actual local
		// authorize, PKCE proof and token handlers without using a retail account.
		flow.codes[sha256.Sum256([]byte(code))] = oauthCode{client: approved.client, challenge: approved.challenge,
			expires: approved.expires, tokenExpiry: time.Now().Add(time.Minute), scopes: approved.scopes,
			token: browserTokens{ID: brokerToken, Refresh: strings.Repeat("1", 64)}}
		form := url.Values{"client_id": {clientID}, "session_token_code": {code}, "session_token_code_verifier": {verifier}}
		if deviceAssertion != "" {
			form.Set("device_authentication_token", deviceAssertion)
		}
		if response := hos19Form(http.HandlerFunc(flow.sessionToken), "/connect/1.0.0/api/session_token", form); response.Code != http.StatusOK {
			t.Fatalf("local PKCE exchange returned HTTP %d", response.Code)
		}
		return code
	}
	exchange := func(code, scope, deviceAssertion string) *httptest.ResponseRecorder {
		form := url.Values{"client_id": {clientID}, "session_token": {code}, "scope": {scope}}
		if deviceAssertion != "" {
			form.Set("device_authentication_token", deviceAssertion)
		}
		return hos19Form(http.HandlerFunc(flow.token), contract.TokenPath, form)
	}
	requested := strings.Join(contract.ProfileScopes, " ")
	response := exchange(authorize(""), requested, "")
	if response.Code != http.StatusOK {
		t.Fatalf("documented native scope transition returned HTTP %d", response.Code)
	}
	var tokens struct {
		Access string   `json:"access_token"`
		ID     string   `json:"id_token"`
		Scope  []string `json:"scope"`
	}
	if json.Unmarshal(response.Body.Bytes(), &tokens) != nil || tokens.Access == "" || tokens.ID == "" ||
		tokens.Access == tokens.ID || !slices.Equal(slices.Sorted(slices.Values(tokens.Scope)), slices.Sorted(slices.Values(contract.ProfileScopes))) {
		t.Fatal("native token exchange did not preserve its approved scopes and credential roles")
	}
	for _, unsupported := range []string{"user.unknown", "user.other[].id"} {
		response := exchange(authorize(""), requested+" "+unsupported, "")
		var failure struct {
			Name        string `json:"error"`
			Description string `json:"error_description"`
		}
		if response.Code != http.StatusBadRequest || json.Unmarshal(response.Body.Bytes(), &failure) != nil ||
			failure.Name != "invalid_scope" || failure.Description == "" {
			t.Fatalf("undocumented profile scope returned HTTP %d", response.Code)
		}
	}

	boundCode := authorize(proofA)
	for _, failure := range []string{"unknown-local-device", gameProof, proofB} {
		response := exchange(boundCode, requested, failure)
		var outcome map[string]string
		if response.Code != http.StatusBadRequest || json.Unmarshal(response.Body.Bytes(), &outcome) != nil || outcome["error"] != "invalid_grant" {
			t.Fatalf("incorrect optional device proof returned HTTP %d", response.Code)
		}
	}
	response = exchange(boundCode, requested, proofA)
	if response.Code != http.StatusOK || json.Unmarshal(response.Body.Bytes(), &tokens) != nil ||
		hos19JWTPart(t, tokens.ID, 1)["rc:device"] != naDeviceIdentity(deviceA) {
		t.Fatal("the issued subject did not preserve the correct optional device proof")
	}
	if _, status := flow.a.federationGrant(t.Context(), tokens.ID, deviceB); status != http.StatusForbidden {
		t.Fatalf("device-bound ID token could federate on another enrolled device, status %d", status)
	}
	if owner, status := flow.a.federationGrant(t.Context(), tokens.ID, deviceA); status != http.StatusOK || owner.Account != player.Account {
		t.Fatalf("the original enrolled owner could not federate, status %d", status)
	}
}
