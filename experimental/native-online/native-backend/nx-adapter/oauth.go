package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"html/template"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"slices"
	"strings"
	"sync"
	"time"

	"roomcircuit.local/nx-adapter/wire"
)

type oauthFlow struct {
	client    string
	challenge string
	state     string
	nonce     string
	scopes    []string
	expires   time.Time
	verifying bool
}

type oauthCode struct {
	client      string
	challenge   string
	expires     time.Time
	tokenExpiry time.Time
	verified    bool
	token       browserTokens
	nonce       string
	scopes      []string
	device      deviceKey
}

type browserTokens struct {
	ID      string `json:"id_token"`
	Access  string `json:"access_token"`
	Refresh string `json:"refresh_token"`
	Expires int    `json:"expires_in"`
}

type linkOAuth struct {
	a               *adapter
	client          *http.Client
	browser         string
	account         string
	usernameRealm   bool
	registrationURL string
	mu              sync.Mutex
	flows           map[string]oauthFlow
	codes           map[[32]byte]oauthCode
	rates           map[string]oauthRate
}

type oauthRate struct {
	started time.Time
	count   int
}

// The socket peer is authoritative. A TLS-passthrough proxy may aggregate
// clients at one IP; never trust an unauthenticated forwarding header.
func (s *linkOAuth) allowBrowser(r *http.Request, stage string, max int) bool {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	ip := net.ParseIP(host)
	if err != nil || ip == nil {
		return false
	}
	key, now := stage+":"+ip.String(), time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.rates == nil {
		s.rates = make(map[string]oauthRate)
	}
	if len(s.rates) >= 4096 {
		for k, item := range s.rates {
			if now.Sub(item.started) >= 15*time.Minute {
				delete(s.rates, k)
			}
		}
		if len(s.rates) >= 4096 {
			return false
		}
	}
	item := s.rates[key]
	if item.started.IsZero() || now.Sub(item.started) >= 15*time.Minute {
		item = oauthRate{started: now}
	}
	if item.count >= max {
		return false
	}
	item.count++
	s.rates[key] = item
	return true
}

var linkForm = template.Must(template.New("link").Parse(`<!doctype html><html lang="en"><meta charset="utf-8"><title>Link this Switch to RoomCircuit</title><main><h1>Link this Switch to RoomCircuit</h1><p>Sign in once for supported online games.</p><form method="post" action="/connect/1.0.0/authorize"><input type="hidden" name="form_token" value="{{.Token}}">{{if .UsernameRealm}}<label>Username <input name="username" autocomplete="username" required></label>{{else}}<label>Email <input type="email" name="email" autocomplete="username" required></label>{{end}}<br><label>Password <input type="password" name="password" autocomplete="current-password" required></label><br><button>Approve device</button></form>{{if .RegistrationURL}}<p><a href="{{.RegistrationURL}}">Create account</a></p>{{end}}</main></html>`))
var verifierPattern = regexp.MustCompile(`^[A-Za-z0-9._~-]{43,128}$`)
var oauthScope = regexp.MustCompile(`^[A-Za-z0-9_.:/-]{1,64}$`)
var nativeOAuthClientID = regexp.MustCompile(`^[0-9a-f]{16}$`)

const nxAccountClient = "6ffd70c434d303c8"
const nxAccountRedirect = "nintendo://account.nx.sys"

var nxProfileScopes = []string{"openid", "offline", "napps", "urn:oauth:init-sso", "user", "user.birthday", "user.email",
	"user.links", "user.links[].id", "user.loginId", "user.screenName", "user.terms"}

func naError(w http.ResponseWriter, status int, code string) {
	description := "The account service could not complete the request."
	switch code {
	case "invalid_oauth_request", "invalid_request":
		code, description = "invalid_request", "The request does not match the registered account flow."
	case "invalid_grant":
		description = "The account authorization is invalid, expired or revoked."
	case "invalid_scope", "insufficient_scope":
		description = "The account authorization does not cover the requested profile fields."
	case "invalid_token":
		description = "The account access token is invalid or expired."
	case "invalid_browser_session", "account_link_not_approved":
		code, description = "access_denied", "Account linking requires a verified sign-in and browser approval."
	case "complete_account_profile":
		code, description = "interaction_required", "Complete your account profile before continuing."
	case "account_service_unavailable", "issuer_unavailable":
		code, description = "server_error", "The account service is temporarily unavailable."
	case "too_many_link_requests":
		description = "Too many account link requests. Try again later."
	}
	respond(w, status, map[string]string{"error": code, "error_description": description})
}

func (n *naIssuer) nativeAccountClient(client string) bool {
	redirect := n.audiences[client]
	if redirect == nil {
		return false
	}
	return (client == nxAccountClient && redirect.String() == nxAccountRedirect) ||
		(nativeOAuthClientID.MatchString(client) && redirect.String() == "npf"+client+"://auth")
}

func requestedScopes(raw string) ([]string, bool) {
	if len(raw) > 512 {
		return nil, false
	}
	parts := strings.Fields(strings.ReplaceAll(raw, "+", " "))
	if len(parts) > 24 {
		return nil, false
	}
	scopes := make([]string, 0, len(parts))
	for _, part := range parts {
		if part != "user.links[].id" && !oauthScope.MatchString(part) {
			return nil, false
		}
		if !slices.Contains(scopes, part) {
			scopes = append(scopes, part)
		}
	}
	return scopes, true
}

func (s *linkOAuth) prune(now time.Time) {
	for key, item := range s.flows {
		if !now.Before(item.expires) {
			delete(s.flows, key)
		}
	}
	for key, item := range s.codes {
		if !now.Before(item.expires) {
			delete(s.codes, key)
		}
	}
}

func (s *linkOAuth) begin(w http.ResponseWriter, r *http.Request) {
	if !s.allowBrowser(r, "begin", 120) {
		naError(w, 429, "too_many_link_requests")
		return
	}
	q := r.URL.Query()
	client := q.Get("client_id")
	redirect := s.a.na.audiences[client]
	challenge, method := q.Get("session_token_code_challenge"), q.Get("session_token_code_challenge_method")
	if challenge == "" {
		challenge, method = q.Get("code_challenge"), q.Get("code_challenge_method")
	}
	decoded, err := base64.RawURLEncoding.DecodeString(challenge)
	scopes, validScopes := requestedScopes(q.Get("scope"))
	if redirect == nil || q.Get("redirect_uri") != redirect.String() || q.Get("response_type") != "session_token_code" ||
		method != "S256" || err != nil || len(decoded) != 32 || q.Get("state") == "" || len(q.Get("state")) > 128 || len(q.Get("nonce")) > 128 || !validScopes {
		naError(w, 400, "invalid_oauth_request")
		return
	}
	if s.a.na.nativeAccountClient(client) && !slices.Equal(scopes, []string{"nx"}) {
		naError(w, 400, "invalid_scope")
		return
	}
	form := randomHex(24)
	now := time.Now()
	s.mu.Lock()
	s.prune(now)
	if len(s.flows)+len(s.codes) >= 128 {
		s.mu.Unlock()
		naError(w, 429, "too_many_link_requests")
		return
	}
	s.flows[form] = oauthFlow{client: client, challenge: challenge, state: q.Get("state"), nonce: q.Get("nonce"), scopes: scopes, expires: now.Add(5 * time.Minute)}
	s.mu.Unlock()
	http.SetCookie(w, &http.Cookie{Name: "rc_nx_link", Value: form, Path: "/connect/1.0.0/authorize", Secure: true, HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: 300})
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
	_ = linkForm.Execute(w, map[string]any{"Token": form, "UsernameRealm": s.usernameRealm, "RegistrationURL": s.registrationURL})
}

func (s *linkOAuth) call(ctx context.Context, path, mime, cookie string, body []byte) (*http.Response, error) {
	method := "GET"
	if body != nil {
		method = "POST"
	}
	req, err := http.NewRequestWithContext(ctx, method, s.browser+path, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	if mime != "" {
		req.Header.Set("Content-Type", mime)
	}
	if cookie != "" {
		req.Header.Set("Cookie", cookie)
		req.Header.Set("Origin", s.browser)
	}
	if s.usernameRealm {
		req.Header.Set("X-Internal-Key", s.a.grantKey)
	}
	return s.client.Do(req)
}

// The username realm issues a core session on every login. The OAuth bridge
// uses it only for the account ID and must not consume a long-lived session
// slot after approval, including when the caller has already disconnected.
func (s *linkOAuth) closeUsernameSession(value string) {
	if !s.usernameRealm || value == "" {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "POST", s.account+"/v1/logout", nil)
	if err != nil {
		return
	}
	req.AddCookie(&http.Cookie{Name: "openpak_session", Value: value})
	res, err := s.a.client.Do(req)
	if err == nil {
		_, _ = io.Copy(io.Discard, io.LimitReader(res.Body, 1024))
		res.Body.Close()
	}
}

func (s *linkOAuth) brokerApproval(ctx context.Context, loginName, password string) (browserTokens, int) {
	var credentials browserTokens
	loginField := "email"
	if s.usernameRealm {
		loginField = "username"
	}
	loginBody, _ := json.Marshal(map[string]string{loginField: loginName, "password": password})
	loginRequest, err := http.NewRequestWithContext(ctx, "POST", s.account+"/v1/login", bytes.NewReader(loginBody))
	if err != nil {
		return credentials, 503
	}
	loginRequest.Header.Set("Content-Type", "application/json")
	login, err := s.a.client.Do(loginRequest)
	if err != nil {
		return credentials, 503
	}
	if s.usernameRealm {
		for _, cookie := range login.Cookies() {
			if cookie.Name == "openpak_session" && cookie.Value != "" {
				defer s.closeUsernameSession(cookie.Value)
				break
			}
		}
	}
	var profile struct {
		Account string `json:"account_id"`
	}
	err = json.NewDecoder(io.LimitReader(login.Body, 4096)).Decode(&profile)
	login.Body.Close()
	if login.StatusCode == 401 {
		return credentials, 401
	}
	if login.StatusCode == 202 {
		return credentials, 403
	}
	if login.StatusCode != 200 || err != nil || profile.Account == "" {
		return credentials, 503
	}
	// Bind this registered client's grant to the verified account identity.
	// Email text and public OAuth state never determine the device identity.
	derived := hmac.New(sha256.New, []byte(s.a.gameKey))
	_, _ = derived.Write([]byte("roomcircuit-oauth-device-v1\x00" + s.a.na.origin + "\x00" + profile.Account))
	data, _ := json.Marshal(map[string]string{"device_key": fmt.Sprintf("%x", derived.Sum(nil)), "scope": "roomcircuit-game", "application_id": s.a.application})
	res, err := s.call(ctx, "/v1/device/authorize", "application/json", "", data)
	if err != nil {
		return credentials, 503
	}
	var pending struct {
		Public  string `json:"user_code"`
		Private string `json:"device_code"`
	}
	err = json.NewDecoder(io.LimitReader(res.Body, 4096)).Decode(&pending)
	res.Body.Close()
	if err != nil || res.StatusCode != 201 || pending.Public == "" || pending.Private == "" {
		return credentials, 503
	}
	page, err := s.call(ctx, "/device?"+url.Values{"user_code": {pending.Public}}.Encode(), "", "", nil)
	if err != nil {
		return credentials, 503
	}
	page.Body.Close()
	var csrf string
	for _, item := range page.Cookies() {
		if item.Name == "rc_device_form" {
			csrf = item.Value
			break
		}
	}
	if page.StatusCode != 200 || csrf == "" {
		return credentials, 503
	}
	form := url.Values{"user_code": {pending.Public}, "csrf_token": {csrf}, loginField: {loginName}, "password": {password}, "decision": {"approve"}}
	approved, err := s.call(ctx, "/device", "application/x-www-form-urlencoded", "rc_device_form="+csrf, []byte(form.Encode()))
	if err != nil {
		return credentials, 503
	}
	approved.Body.Close()
	if approved.StatusCode == 401 || approved.StatusCode == 403 || approved.StatusCode == 409 {
		return credentials, approved.StatusCode
	}
	if approved.StatusCode != 200 {
		return credentials, 503
	}
	data, _ = json.Marshal(map[string]string{"device_code": pending.Private})
	res, err = s.call(ctx, "/v1/device/token", "application/json", "", data)
	if err != nil {
		return credentials, 503
	}
	defer res.Body.Close()
	if res.StatusCode != 200 || json.NewDecoder(io.LimitReader(res.Body, 8192)).Decode(&credentials) != nil || credentials.ID == "" || credentials.Access == "" || credentials.ID == credentials.Access || credentials.Expires <= 0 {
		return browserTokens{}, 503
	}
	_, status := s.a.checkGrant(ctx, credentials.ID)
	return credentials, status
}

func (s *linkOAuth) approve(w http.ResponseWriter, r *http.Request) {
	if !s.allowBrowser(r, "approve", 20) {
		naError(w, 429, "too_many_link_requests")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil {
		naError(w, 400, "invalid_request")
		return
	}
	cookie, err := r.Cookie("rc_nx_link")
	if err != nil || subtle.ConstantTimeCompare([]byte(cookie.Value), []byte(r.PostForm.Get("form_token"))) != 1 || (r.Header.Get("Origin") != "" && r.Header.Get("Origin") != s.a.na.origin) {
		naError(w, 403, "invalid_browser_session")
		return
	}
	loginField := "email"
	if s.usernameRealm {
		loginField = "username"
	}
	loginName, password := r.PostForm.Get(loginField), r.PostForm.Get("password")
	if loginName == "" || password == "" || len(password) > 256 {
		naError(w, 400, "invalid_request")
		return
	}
	s.mu.Lock()
	s.prune(time.Now())
	flow, ok := s.flows[cookie.Value]
	if !ok || flow.verifying {
		s.mu.Unlock()
		naError(w, 400, "invalid_grant")
		return
	}
	flow.verifying = true
	s.flows[cookie.Value] = flow
	s.mu.Unlock()
	credentials, status := s.brokerApproval(r.Context(), loginName, password)
	s.mu.Lock()
	delete(s.flows, cookie.Value)
	if status != 200 || !time.Now().Before(flow.expires) {
		s.mu.Unlock()
		if status == 200 {
			status = 400
		}
		naError(w, status, "account_link_not_approved")
		return
	}
	code := randomHex(32)
	expires := time.Now().Add(2 * time.Minute)
	if expires.After(flow.expires) {
		expires = flow.expires
	}
	s.codes[sha256.Sum256([]byte(code))] = oauthCode{client: flow.client, challenge: flow.challenge, expires: expires,
		tokenExpiry: time.Now().Add(time.Duration(credentials.Expires) * time.Second), token: credentials, nonce: flow.nonce, scopes: flow.scopes}
	s.mu.Unlock()
	destination := *s.a.na.audiences[flow.client]
	q := destination.Query()
	q.Set("session_token_code", code)
	q.Set("state", flow.state)
	destination.RawQuery = q.Encode()
	w.Header().Set("Cache-Control", "no-store")
	http.Redirect(w, r, destination.String(), http.StatusFound)
}

func (s *linkOAuth) sessionToken(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil || s.a.na.audiences[r.PostForm.Get("client_id")] == nil {
		naError(w, 400, "invalid_request")
		return
	}
	device, verifiedDevice := s.optionalAccountDevice(r)
	if !verifiedDevice {
		naError(w, 400, "invalid_grant")
		return
	}
	code, verifier := r.PostForm.Get("session_token_code"), r.PostForm.Get("session_token_code_verifier")
	if !verifierPattern.MatchString(verifier) {
		naError(w, 400, "invalid_grant")
		return
	}
	hash := sha256.Sum256([]byte(code))
	proof := sha256.Sum256([]byte(verifier))
	s.mu.Lock()
	s.prune(time.Now())
	grant, ok := s.codes[hash]
	if !ok || grant.client != r.PostForm.Get("client_id") || grant.verified || subtle.ConstantTimeCompare([]byte(base64.RawURLEncoding.EncodeToString(proof[:])), []byte(grant.challenge)) != 1 {
		s.mu.Unlock()
		naError(w, 400, "invalid_grant")
		return
	}
	grant.verified = true
	grant.device = device
	// The native client repeats this code at the token endpoint. Restrict
	// that final exchange to the short interval after successful S256 proof.
	if exchangeUntil := time.Now().Add(30 * time.Second); exchangeUntil.Before(grant.expires) {
		grant.expires = exchangeUntil
	}
	s.codes[hash] = grant
	s.mu.Unlock()
	respond(w, 200, map[string]string{"session_token": code, "code": code})
}

func (s *linkOAuth) token(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil || s.a.na.audiences[r.PostForm.Get("client_id")] == nil {
		naError(w, 400, "invalid_request")
		return
	}
	if refresh := r.PostForm.Get("refresh_token"); refresh != "" {
		s.renew(w, r, refresh, false)
		return
	}
	device, verifiedDevice := s.optionalAccountDevice(r)
	if !verifiedDevice {
		naError(w, 400, "invalid_grant")
		return
	}
	code := r.PostForm.Get("session_token_code")
	if code == "" {
		code = r.PostForm.Get("code")
	}
	if code == "" {
		code = r.PostForm.Get("session_token")
	}
	hash := sha256.Sum256([]byte(code))
	s.mu.Lock()
	s.prune(time.Now())
	grant, ok := s.codes[hash]
	if !ok || !grant.verified || grant.client != r.PostForm.Get("client_id") ||
		(grant.device != (deviceKey{}) && device != (deviceKey{}) && grant.device != device) {
		s.mu.Unlock()
		naError(w, 400, "invalid_grant")
		return
	}
	if device == (deviceKey{}) {
		device = grant.device
	}
	scopes, valid := tokenScopes(r.PostForm.Get("scope"), grant.scopes, s.a.na.nativeAccountClient(grant.client))
	if !valid {
		s.mu.Unlock()
		naError(w, 400, "invalid_scope")
		return
	}
	delete(s.codes, hash)
	s.mu.Unlock()
	who, status := s.a.checkGrant(r.Context(), grant.token.ID)
	if status != 200 {
		naError(w, status, "invalid_grant")
		return
	}
	seconds := int(time.Until(grant.tokenExpiry).Seconds())
	if seconds <= 0 {
		naError(w, 400, "invalid_grant")
		return
	}
	s.issueNA(w, who, grant.client, grant.token.Refresh, seconds, scopes, grant.nonce, naDeviceIdentity(device), time.Now().Add(naRefreshTTL).Unix())
}

func (s *linkOAuth) optionalAccountDevice(r *http.Request) (deviceKey, bool) {
	values, supplied := r.PostForm["device_authentication_token"]
	if !supplied {
		return deviceKey{}, true
	}
	if len(values) != 1 || values[0] == "" {
		return deviceKey{}, false
	}
	return s.a.bootstrap.accountDeviceAuthorization(values[0])
}

func tokenScopes(request string, approved []string, native bool) ([]string, bool) {
	if request == "" {
		return approved, true
	}
	scopes, ok := requestedScopes(request)
	for _, scope := range scopes {
		if !slices.Contains(approved, scope) && !(native && slices.Contains(approved, "nx") && slices.Contains(nxProfileScopes, scope)) {
			return nil, false
		}
	}
	return scopes, ok
}

func (s *linkOAuth) issueNA(w http.ResponseWriter, who player, client, refreshSecret string, seconds int, scopes []string, nonce, device string, refreshExpiry int64) {
	now := time.Now().Unix()
	if len(who.GrantID) != 48 || len(refreshSecret) != 64 || seconds <= 0 || refreshExpiry <= now {
		naError(w, 503, "account_service_unavailable")
		return
	}
	p := naProof{Issuer: s.a.na.origin, Audience: client, Subject: naSubject(who), Issued: now, Expires: now + int64(seconds),
		JWTID: uuid4(), Grant: who.GrantID, Scopes: scopes, Device: device, Purpose: "token", GrantType: 4}
	if s.a.na.nativeAccountClient(client) && slices.Equal(slices.Sorted(slices.Values(scopes)), slices.Sorted(slices.Values(nxProfileScopes))) {
		p.NativeScopes = []int{0, 1, 4, 5, 8, 9, 10, 11, 12, 13, 23, 28}
	}
	access, err := s.a.na.signed(p)
	if err != nil {
		naError(w, 503, "issuer_unavailable")
		return
	}
	digest := sha256.Sum256([]byte(access))
	p.Purpose, p.JWTID, p.Nonce, p.AccessHash, p.GrantType, p.NativeScopes = "", uuid4(), nonce, jwtEncoding.EncodeToString(digest[:16]), 0, nil
	id, err := s.a.na.signed(p)
	if err != nil {
		naError(w, 503, "issuer_unavailable")
		return
	}
	p.Purpose, p.JWTID, p.Nonce, p.AccessHash = "session", uuid4(), "", ""
	session, err := s.a.na.signed(p)
	if err != nil {
		naError(w, 503, "issuer_unavailable")
		return
	}
	p.Purpose, p.JWTID, p.Expires = "refresh", uuid4(), refreshExpiry
	refresh, err := s.a.na.wrap(p, refreshSecret)
	if err != nil {
		naError(w, 503, "issuer_unavailable")
		return
	}
	respond(w, 200, map[string]any{"id_token": id, "access_token": access, "session_token": session, "refresh_token": refresh,
		"expires_in": seconds, "scope": scopes, "token_type": "Bearer"})
}

func (s *linkOAuth) logout(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil || s.a.na.audiences[r.PostForm.Get("client_id")] == nil {
		naError(w, 400, "invalid_request")
		return
	}
	s.renew(w, r, r.PostForm.Get("refresh_token"), true)
}

func (s *linkOAuth) renew(w http.ResponseWriter, r *http.Request, refresh string, logout bool) {
	p, valid := s.a.na.verify(refresh, "refresh", logout)
	secret, unwrapped := s.a.na.unwrap(p)
	if !valid || !unwrapped || p.Audience != r.PostForm.Get("client_id") {
		naError(w, 400, "invalid_grant")
		return
	}
	device, verifiedDevice := s.optionalAccountDevice(r)
	deviceID := naDeviceIdentity(device)
	if !verifiedDevice || (p.Device != "" && deviceID != "" && p.Device != deviceID) {
		naError(w, 400, "invalid_grant")
		return
	}
	if deviceID == "" {
		deviceID = p.Device
	}
	scopes, valid := tokenScopes(r.PostForm.Get("scope"), p.Scopes, s.a.na.nativeAccountClient(p.Audience))
	if !valid {
		naError(w, 400, "invalid_scope")
		return
	}
	path, expected := "/v1/device/refresh", http.StatusOK
	var who player
	if logout {
		path, expected = "/v1/device/logout", http.StatusNoContent
	} else {
		var status int
		who, status = s.a.naPlayer(r.Context(), p)
		if status != 200 {
			naError(w, status, "invalid_grant")
			return
		}
	}
	data, _ := json.Marshal(map[string]string{"refresh_token": secret, "application_id": s.a.application})
	res, err := s.call(r.Context(), path, "application/json", "", data)
	if err != nil {
		naError(w, 503, "account_service_unavailable")
		return
	}
	defer res.Body.Close()
	if res.StatusCode != expected {
		switch res.StatusCode {
		case 400, 401, 403, 429:
			naError(w, res.StatusCode, "invalid_grant")
		default:
			naError(w, 503, "account_service_unavailable")
		}
		return
	}
	if logout {
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(http.StatusNoContent)
		return
	}
	var token browserTokens
	if json.NewDecoder(io.LimitReader(res.Body, 8192)).Decode(&token) != nil || token.ID == "" || token.Access == "" || token.ID == token.Access || token.Expires <= 0 || len(token.Refresh) != 64 {
		naError(w, 503, "account_service_unavailable")
		return
	}
	s.issueNA(w, who, p.Audience, token.Refresh, token.Expires, scopes, "", deviceID, p.Expires)
}

func (a *adapter) ownProfile(w http.ResponseWriter, r *http.Request) {
	if a.na == nil {
		naError(w, 401, "invalid_token")
		return
	}
	p, ok := a.na.verify(bearer(r), "token", false)
	if !ok {
		naError(w, 401, "invalid_token")
		return
	}
	if a.na.nativeAccountClient(p.Audience) && !slices.Contains(p.Scopes, "nx") &&
		(!slices.Contains(p.Scopes, "user") || !slices.Contains(p.Scopes, "user.email") || !slices.Contains(p.Scopes, "user.birthday")) {
		naError(w, 403, "insufficient_scope")
		return
	}
	who, status := a.naPlayer(r.Context(), p)
	if status != 200 {
		naError(w, status, "invalid_token")
		return
	}
	jsonBody, _ := json.Marshal(map[string]string{"grant_id": who.GrantID, "application_id": a.application})
	req, err := http.NewRequestWithContext(r.Context(), "POST", a.grantURL+"/internal/account/profile", bytes.NewReader(jsonBody))
	if err != nil {
		naError(w, 503, "account_service_unavailable")
		return
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Internal-Key", a.grantKey)
	res, err := a.client.Do(req)
	if err != nil {
		naError(w, 503, "account_service_unavailable")
		return
	}
	defer res.Body.Close()
	switch res.StatusCode {
	case http.StatusOK:
		var profile struct {
			ID                     string `json:"id"`
			Nickname               string `json:"nickname"`
			AccountType            string `json:"account_type"`
			Email                  string `json:"email"`
			Country                string `json:"country"`
			Birthday               string `json:"birthday"`
			EmailVerified          bool   `json:"emailVerified"`
			TermsAgreementRequired bool   `json:"termsAgreementRequired"`
		}
		if json.NewDecoder(io.LimitReader(res.Body, 8192)).Decode(&profile) != nil || profile.ID != who.User {
			naError(w, 503, "account_service_unavailable")
			return
		}
		id := naSubject(who)
		// Only the authenticated private broker can assert a username account.
		// Unrecognized types fail closed instead of relaxing email checks.
		if profile.AccountType != "" && profile.AccountType != "email" && profile.AccountType != "username" {
			naError(w, 503, "account_service_unavailable")
			return
		}
		result, err := wire.BuildNAProfile(wire.NAProfileInput{ID: id, AccessSubject: id, Nickname: profile.Nickname,
			Email: profile.Email, Country: profile.Country, Birthday: profile.Birthday, EmailVerified: profile.EmailVerified,
			UsernameOnly:           profile.AccountType == "username",
			TermsAgreementRequired: profile.TermsAgreementRequired, VerifiedRealmMembership: who.GrantID != ""})
		if err != nil {
			naError(w, 409, "complete_account_profile")
			return
		}
		respond(w, 200, result)
	case 401, 403, 404:
		naError(w, res.StatusCode, "invalid_token")
	case http.StatusConflict:
		naError(w, http.StatusConflict, "complete_account_profile")
	default:
		naError(w, 503, "account_service_unavailable")
	}
}

func (a *adapter) oauth(mux *http.ServeMux) error {
	allowed := os.Getenv("NX_LAB_OAUTH_REDIRECT")
	if allowed == "" {
		return nil
	}
	redirect, err := url.Parse(allowed)
	if err != nil || redirect.Scheme == "" || redirect.Scheme == "http" || redirect.User != nil || redirect.Fragment != "" || redirect.Host == "" {
		return errors.New("invalid registered OAuth redirect")
	}
	ca, err := os.ReadFile(os.Getenv("NX_LAB_BROWSER_CA"))
	if err != nil {
		return err
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(ca) {
		return errors.New("own browser grant service CA is invalid")
	}
	originString := os.Getenv("NX_LAB_BROWSER_ORIGIN")
	brokerOrigin, ok := origin(originString, "https")
	if !ok || !net.ParseIP(brokerOrigin.Hostname()).IsLoopback() {
		return errors.New("browser grant service requires a trusted HTTPS origin")
	}
	accountURL := os.Getenv("NX_LAB_ACCOUNT_URL")
	if parsed, ok := origin(accountURL, "http"); !ok || !net.ParseIP(parsed.Hostname()).IsLoopback() {
		return errors.New("account login endpoint must remain loopback")
	}
	client := os.Getenv("NX_LAB_OAUTH_CLIENT_ID")
	if !oauthScope.MatchString(client) {
		return errors.New("registered OAuth client id is missing")
	}
	audiences := map[string]*url.URL{client: redirect}
	if client == nxAccountClient && redirect.String() != nxAccountRedirect {
		return errors.New("NxAccount client requires its registered system callback")
	}
	if a.bootstrap != nil {
		audiences[nxAccountClient] = &url.URL{Scheme: "nintendo", Host: "account.nx.sys"}
	}
	if title := os.Getenv("NX_LAB_TITLE_OAUTH_CLIENT_ID"); title != "" {
		if !nativeOAuthClientID.MatchString(title) {
			return errors.New("title OAuth client ID requires 16 hexadecimal characters")
		}
		titleRedirect := &url.URL{Scheme: "npf" + title, Host: "auth"}
		if known := audiences[title]; known != nil && known.String() != titleRedirect.String() {
			return errors.New("title OAuth redirect does not match its registered client")
		}
		audiences[title] = titleRedirect
	}
	naOrigin := os.Getenv("NX_LAB_NA_ISSUER")
	naEndpoint, valid := origin(naOrigin, "https")
	if !valid || naOrigin == originString {
		return errors.New("NA issuer requires its own HTTPS origin")
	}
	naKey, err := key(os.Getenv("NX_LAB_NA_SIGNING_KEY"))
	if err != nil {
		return err
	}
	if naKey.N.Cmp(a.rsaKey.N) == 0 {
		return errors.New("NA and BAAS signing keys must differ")
	}
	a.na, err = newNAIssuer(naOrigin, a.grantKey, audiences, naKey)
	if err != nil {
		return err
	}
	realm := os.Getenv("NX_LAB_ACCOUNT_REALM")
	if realm != "" && realm != "email" && realm != "username" {
		return errors.New("account realm must be email or username")
	}
	registration := os.Getenv("NX_LAB_REGISTRATION_URL")
	if registration != "" {
		u, err := url.Parse(registration)
		if realm != "username" || err != nil || u.Scheme != "https" || u.Hostname() == "" || u.User != nil ||
			u.Path != "/register" || u.RawQuery != "" || u.Fragment != "" || strings.EqualFold(u.Hostname(), "localhost") || net.ParseIP(u.Hostname()).IsLoopback() {
			return errors.New("registration URL must be an own reachable HTTPS /register endpoint")
		}
	}
	s := &linkOAuth{a: a, browser: originString, account: accountURL, usernameRealm: realm == "username", registrationURL: registration,
		client: &http.Client{Timeout: 8 * time.Second, Transport: &http.Transport{TLSClientConfig: &tls.Config{RootCAs: pool, MinVersion: tls.VersionTLS12}}, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},
		flows:  make(map[string]oauthFlow), codes: make(map[[32]byte]oauthCode)}
	mux.HandleFunc("GET /connect/1.0.0/authorize", s.begin)
	mux.HandleFunc("POST /connect/1.0.0/authorize", s.approve)
	mux.HandleFunc("POST /connect/1.0.0/api/session_token", s.sessionToken)
	mux.HandleFunc("POST /connect/1.0.0/api/token", s.token)
	mux.HandleFunc("POST /connect/1.0.0/api/logout", s.logout)
	mux.HandleFunc("GET /2.0.0/users/me", a.ownProfile)
	certificates := "GET /1.0.0/certificates"
	if a.bootstrap != nil {
		certificates = "GET " + naEndpoint.Hostname() + "/1.0.0/certificates"
	}
	mux.HandleFunc(certificates, a.na.certificates)
	return nil
}
