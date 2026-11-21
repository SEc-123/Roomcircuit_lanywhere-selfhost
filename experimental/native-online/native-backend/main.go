package main

import (
	"bytes"
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"html/template"
	"io"
	"log"
	"maps"
	"net"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"
)

const (
	scope        = "roomcircuit-game"
	challengeTTL = 5 * time.Minute
	pollInterval = 2 * time.Second
	tokenTTL     = 15 * time.Minute
)

var applicationIDPattern = regexp.MustCompile(`^[0-9a-f]{16}$`)

type challenge struct {
	deviceKey   string
	deviceCode  [32]byte
	application string
	state       string
	expires     time.Time
	lastPoll    time.Time
	approved    approvedDevice
}

type grant struct {
	id          string
	application string
	pid         uint64
	expires     time.Time
}

type claims struct {
	Issuer      string `json:"iss"`
	Audience    string `json:"aud"`
	Purpose     string `json:"typ"`
	Subject     string `json:"sub"`
	ID          string `json:"jti"`
	Grant       string `json:"grant_id,omitempty"`
	Application string `json:"application_id,omitempty"`
	Expires     int64  `json:"exp"`
}

type service struct {
	issuerURL     string
	accountURL    string
	linksURL      string
	linksKey      string
	gameKey       string
	signing       *rsa.PrivateKey
	client        *http.Client
	mu            sync.Mutex
	pending       map[string]*challenge
	grants        map[string]grant
	durable       *durableGrants
	authority     *accountAuthority
	applications  map[string]bool
	usernameRealm bool
	edge          edgeLimiter
}

func (s *service) supportedApplication(id string) bool {
	return id == "" || s.applications[id]
}

// service.mu is held when publishing the newly issued session reference.
func (s *service) rememberIdentity(identity claims, pid uint64) {
	s.grants[identity.ID] = grant{identity.Grant, identity.Application, pid, time.Unix(identity.Expires, 0)}
}

func randomHex(bytes int) string {
	result := make([]byte, bytes)
	if _, err := rand.Read(result); err != nil {
		panic(err)
	}
	return hex.EncodeToString(result)
}

func equal(a, b string) bool {
	return a != "" && subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

func response(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

func reject(w http.ResponseWriter, status int, message string) {
	response(w, status, map[string]string{"error": message})
}

func request(w http.ResponseWriter, r *http.Request, data any) bool {
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	decoder.DisallowUnknownFields()
	if decoder.Decode(data) != nil || !errors.Is(decoder.Decode(new(any)), io.EOF) {
		reject(w, 400, "invalid_request")
		return false
	}
	return true
}

func (s *service) prune(now time.Time) {
	for code, item := range s.pending {
		if !now.Before(item.expires) {
			delete(s.pending, code)
		}
	}
	for id, item := range s.grants {
		if !now.Before(item.expires) {
			delete(s.grants, id)
		}
	}
}

func (s *service) authorize(w http.ResponseWriter, r *http.Request) {
	var input struct {
		DeviceKey   string `json:"device_key"`
		Scope       string `json:"scope"`
		Application string `json:"application_id"`
	}
	if !request(w, r, &input) {
		return
	}
	key, err := hex.DecodeString(input.DeviceKey)
	if err != nil || len(key) != 32 || input.Scope != scope {
		reject(w, 400, "invalid_request")
		return
	}
	if !s.supportedApplication(input.Application) {
		reject(w, 403, "wrong_application")
		return
	}
	device := randomHex(32)
	now := time.Now()
	s.mu.Lock()
	s.prune(now)
	if len(s.pending) >= 128 {
		s.mu.Unlock()
		reject(w, 429, "too_many_requests")
		return
	}
	code := strings.ToUpper(randomHex(4))
	for s.pending[code] != nil {
		code = strings.ToUpper(randomHex(4))
	}
	s.pending[code] = &challenge{deviceKey: hex.EncodeToString(key), deviceCode: sha256.Sum256([]byte(device)), application: input.Application, state: "pending", expires: now.Add(challengeTTL)}
	s.mu.Unlock()
	page := s.issuerURL + "/device"
	response(w, 201, map[string]any{"device_code": device, "user_code": code, "verification_uri": page, "verification_uri_complete": page + "?user_code=" + code, "expires_in": int(challengeTTL / time.Second), "interval": int(pollInterval / time.Second)})
}

var devicePage = template.Must(template.New("device").Parse(`<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Link this Switch to RoomCircuit</title><main><h1>Link this Switch to RoomCircuit</h1><p>Confirm the code on your Switch to sign in once for supported online games.</p><form method="post" action="/device"><input type="hidden" name="csrf_token" value="{{.CSRF}}"><label>Device code <input name="user_code" value="{{.Code}}" required autocomplete="off"></label><br>{{if .Username}}<label>Username <input name="username" autocomplete="username" required></label>{{else}}<label>Email <input name="email" type="email" autocomplete="username" required></label>{{end}}<br><label>Password <input name="password" type="password" autocomplete="current-password" required></label><br><button name="decision" value="approve">Approve device</button> <button name="decision" value="deny">Deny</button></form>{{if .Username}}<p><a href="/register">Create account</a></p>{{end}}</main></html>`))

func (s *service) browser(w http.ResponseWriter, r *http.Request) {
	csrf := randomHex(24)
	http.SetCookie(w, &http.Cookie{Name: "rc_device_form", Value: csrf, Path: "/device", MaxAge: int(challengeTTL / time.Second), SameSite: http.SameSiteStrictMode, HttpOnly: true, Secure: true})
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
	_ = devicePage.Execute(w, map[string]any{"Code": r.URL.Query().Get("user_code"), "CSRF": csrf, "Username": s.usernameRealm})
}

func (s *service) relay(r *http.Request, path string, data any, header string, key string) (*http.Response, error) {
	encoded, err := json.Marshal(data)
	if err != nil {
		return nil, err
	}
	next, err := http.NewRequestWithContext(r.Context(), "POST", path, bytes.NewReader(encoded))
	if err != nil {
		return nil, err
	}
	next.Header.Set("Content-Type", "application/json")
	if header != "" {
		next.Header.Set(header, key)
	}
	return s.client.Do(next)
}

func (s *service) approve(w http.ResponseWriter, r *http.Request) {
	if s.usernameRealm && !s.trustedInternal(r) && !s.edge.allow(r, "login", 100) {
		reject(w, 429, "rate_limited")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if err := r.ParseForm(); err != nil {
		reject(w, 400, "invalid_request")
		return
	}
	csrf, err := r.Cookie("rc_device_form")
	if err != nil || !equal(csrf.Value, r.PostForm.Get("csrf_token")) {
		reject(w, 403, "invalid_browser_session")
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" && origin != s.issuerURL {
		reject(w, 403, "invalid_browser_origin")
		return
	}
	code := strings.ToUpper(strings.TrimSpace(r.PostForm.Get("user_code")))
	decision := r.PostForm.Get("decision")
	if decision != "approve" && decision != "deny" {
		reject(w, 400, "invalid_request")
		return
	}
	s.mu.Lock()
	s.prune(time.Now())
	item := s.pending[code]
	if item == nil || item.state != "pending" {
		s.mu.Unlock()
		reject(w, 400, "expired_token")
		return
	}
	item.state = "verifying"
	key := item.deviceKey
	s.mu.Unlock()
	settled := false
	defer func() {
		s.mu.Lock()
		defer s.mu.Unlock()
		if !settled && s.pending[code] == item {
			item.state = "pending"
		}
	}()
	name, password := r.PostForm.Get("email"), r.PostForm.Get("password")
	credentialField := "email"
	if s.usernameRealm {
		name = r.PostForm.Get("username")
		credentialField = "username"
	}
	if name == "" || password == "" || len(password) > 256 {
		reject(w, 400, "invalid_request")
		return
	}
	login, err := s.relay(r, s.accountURL+"/v1/login", map[string]string{credentialField: name, "password": password}, "", "")
	if err != nil {
		reject(w, 503, "account_unavailable")
		return
	}
	defer login.Body.Close()
	var session string
	for _, c := range login.Cookies() {
		if c.Name == "openpak_session" {
			session = c.Value
			break
		}
	}
	if s.usernameRealm && session != "" {
		defer s.logoutTemporaryUsernameSession(session)
	}
	if login.StatusCode == 202 {
		reject(w, 403, "second_factor_required")
		return
	}
	if login.StatusCode != 200 {
		reject(w, 401, "invalid_credentials")
		return
	}
	var profile struct {
		Account string `json:"account_id"`
	}
	if json.NewDecoder(io.LimitReader(login.Body, 4096)).Decode(&profile) != nil || profile.Account == "" {
		reject(w, 503, "account_unavailable")
		return
	}
	if session == "" {
		reject(w, 503, "account_unavailable")
		return
	}
	if decision == "deny" {
		s.mu.Lock()
		item.state = "denied"
		item.deviceKey = ""
		settled = true
		s.mu.Unlock()
		response(w, 200, map[string]string{"status": "denied"})
		return
	}
	device, err := hex.DecodeString(key)
	if err != nil || len(device) != 32 {
		reject(w, 503, "invalid_device_link")
		return
	}
	subject := sha256.Sum256(append([]byte(ownLinkNamespace+":"), device...))
	approved := approvedDevice{Account: profile.Account, Subject: hex.EncodeToString(subject[:])}
	binding := linkBindingKey(approved.Account, approved.Subject)
	s.mu.Lock()
	eventErr := s.syncRevocations(r.Context())
	approved.LinkVersion = s.durable.revoked[binding]
	s.mu.Unlock()
	if eventErr != nil {
		reject(w, 503, "account_revocations_unavailable")
		return
	}
	fl, err := s.relay(r, s.linksURL+"/v1/devices/link", map[string]string{"device_key": key}, "Authorization", "Bearer "+session)
	if err != nil {
		reject(w, 503, "device_link_unavailable")
		return
	}
	defer fl.Body.Close()
	if fl.StatusCode == 403 || fl.StatusCode == 409 {
		reject(w, fl.StatusCode, "device_link_denied")
		return
	}
	var bind struct {
		Token string `json:"game_token"`
		PID   uint64 `json:"pid"`
	}
	if fl.StatusCode != 201 || json.NewDecoder(io.LimitReader(fl.Body, 4096)).Decode(&bind) != nil || bind.Token == "" || bind.PID == 0 {
		reject(w, 503, "device_link_unavailable")
		return
	}
	approved.PID = bind.PID
	if _, status, _ := s.authority.verify(r.Context(), approved); status != 200 {
		reject(w, status, "device_link_denied")
		return
	}
	s.mu.Lock()
	eventErr = s.syncRevocations(r.Context())
	revoked := s.durable.revoked[binding] > approved.LinkVersion
	if eventErr != nil || revoked {
		s.mu.Unlock()
		if revoked {
			reject(w, 403, "device_link_revoked")
		} else {
			reject(w, 503, "account_revocations_unavailable")
		}
		return
	}
	expired := s.pending[code] != item || !time.Now().Before(item.expires)
	if !expired {
		item.state = "approved"
		item.approved = approved
		item.deviceKey = ""
		settled = true
	}
	s.mu.Unlock()
	if expired {
		reject(w, 400, "expired_token")
		return
	}
	response(w, 200, map[string]string{"status": "approved"})
}

var jwtEncoding = base64.RawURLEncoding
var jwtHeader = jwtEncoding.EncodeToString([]byte(`{"alg":"RS256","typ":"JWT"}`))

func (s *service) sign(c claims) (string, error) {
	body, err := json.Marshal(c)
	if err != nil {
		return "", err
	}
	unsigned := jwtHeader + "." + jwtEncoding.EncodeToString(body)
	digest := sha256.Sum256([]byte(unsigned))
	signature, err := rsa.SignPKCS1v15(rand.Reader, s.signing, crypto.SHA256, digest[:])
	if err != nil {
		return "", err
	}
	return unsigned + "." + jwtEncoding.EncodeToString(signature), nil
}

func (s *service) verify(encoded string, now time.Time) (claims, bool) {
	return s.verifyPurpose(encoded, "id_token", now)
}

func (s *service) verifyPurpose(encoded, purpose string, now time.Time) (claims, bool) {
	var c claims
	parts := strings.Split(encoded, ".")
	if len(parts) != 3 || parts[0] != jwtHeader || len(encoded) > 2048 {
		return c, false
	}
	signature, err := jwtEncoding.DecodeString(parts[2])
	if err != nil {
		return c, false
	}
	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	if rsa.VerifyPKCS1v15(&s.signing.PublicKey, crypto.SHA256, digest[:], signature) != nil {
		return c, false
	}
	body, err := jwtEncoding.DecodeString(parts[1])
	if err != nil || json.Unmarshal(body, &c) != nil {
		return c, false
	}
	return c, c.Issuer == s.issuerURL && c.Audience == scope && c.Purpose == purpose && c.Expires > now.Unix() && c.ID != ""
}

func (s *service) token(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Code string `json:"device_code"`
	}
	if !request(w, r, &input) {
		return
	}
	hash := sha256.Sum256([]byte(input.Code))
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	s.prune(now)
	var code string
	var item *challenge
	for k, v := range s.pending {
		if v.deviceCode == hash {
			code, item = k, v
			break
		}
	}
	if item == nil {
		reject(w, 400, "expired_token")
		return
	}
	if item.state == "denied" {
		delete(s.pending, code)
		reject(w, 400, "access_denied")
		return
	}
	if !item.lastPoll.IsZero() && now.Sub(item.lastPoll) < pollInterval {
		reject(w, 400, "slow_down")
		return
	}
	item.lastPoll = now
	if item.state != "approved" {
		reject(w, 400, "authorization_pending")
		return
	}
	if !s.supportedApplication(item.application) {
		reject(w, 403, "wrong_application")
		return
	}
	if _, status, _ := s.authority.verify(r.Context(), item.approved); status != 200 {
		reject(w, status, "device_link_denied")
		return
	}
	if s.syncRevocations(r.Context()) != nil {
		reject(w, 503, "account_revocations_unavailable")
		return
	}
	if s.durable.revoked[linkBindingKey(item.approved.Account, item.approved.Subject)] > item.approved.LinkVersion {
		delete(s.pending, code)
		reject(w, 403, "device_link_revoked")
		return
	}
	now = time.Now()
	if !now.Before(item.expires) {
		delete(s.pending, code)
		reject(w, 400, "expired_token")
		return
	}
	next := maps.Clone(s.durable.active)
	var id string
	for candidate, entry := range next {
		if entry.Account == item.approved.Account && entry.Subject == item.approved.Subject && entry.PID == item.approved.PID && entry.LinkVersion == item.approved.LinkVersion {
			id = candidate
			break
		}
	}
	if id == "" && len(next) >= maxDurableGrants {
		reject(w, 429, "too_many_requests")
		return
	}
	if id == "" {
		id = randomHex(24)
	}
	refresh := randomHex(32)
	approved := item.approved
	approved.RefreshSHA256 = sha256.Sum256([]byte(refresh))
	approved.RefreshUntil = now.Add(refreshTTL)
	identity, jwt, access, err := s.signedPair(id, approved, item.application, now)
	if err != nil {
		reject(w, 503, "issuer_unavailable")
		return
	}
	next[id] = approved
	if !s.durable.commit(next) {
		reject(w, 503, "grant_store_unavailable")
		return
	}
	s.rememberIdentity(identity, approved.PID)
	delete(s.pending, code)
	returnCredentials(w, approved, jwt, access, refresh)
}

func (s *service) gameIdentity(w http.ResponseWriter, r *http.Request) {
	if !equal(r.Header.Get("X-Internal-Key"), s.gameKey) {
		reject(w, 401, "invalid_internal_key")
		return
	}
	var input struct {
		Token       string `json:"id_token"`
		PID         uint64 `json:"pid"`
		Application string `json:"application_id"`
	}
	if !request(w, r, &input) {
		return
	}
	if !s.supportedApplication(input.Application) {
		reject(w, 403, "wrong_application")
		return
	}
	now := time.Now()
	var current grant
	s.mu.Lock()
	s.prune(now)
	if input.Token != "" && input.PID == 0 {
		if c, ok := s.verify(input.Token, now); ok {
			approved, exists := s.durable.active[c.Grant]
			if exists && fmt.Sprintf("%016x", approved.PID) == c.Subject {
				current = grant{c.Grant, c.Application, approved.PID, time.Unix(c.Expires, 0)}
			}
		}
	} else if input.Token == "" && input.PID != 0 {
		for _, v := range s.grants {
			if v.pid == input.PID && v.application == input.Application && v.expires.After(current.expires) {
				current = v
			}
		}
	}
	approved, approvedOK := s.durable.active[current.id]
	s.mu.Unlock()
	if current.pid == 0 {
		if input.Token == "" && input.PID != 0 {
			reject(w, 404, "unknown_player")
		} else {
			reject(w, 401, "invalid_identity_token")
		}
		return
	}
	if current.application != input.Application {
		reject(w, 403, "wrong_application")
		return
	}
	if !approvedOK || approved.PID != current.pid {
		reject(w, 403, "linked_account_unavailable")
		return
	}
	value, status := s.liveGrant(r.Context(), current.id, approved, current.application)
	if status != 200 {
		reject(w, status, "linked_account_unavailable")
		return
	}
	response(w, 200, value)
}

func loadKey(path string) (*rsa.PrivateKey, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return nil, errors.New("own issuer key is invalid")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, err
	}
	key, ok := parsed.(*rsa.PrivateKey)
	if !ok || key.N.BitLen() < 2048 {
		return nil, errors.New("own issuer requires RSA-2048 or larger")
	}
	return key, key.Validate()
}

func loopbackHTTPOrigin(raw string) bool {
	parsed, err := url.Parse(raw)
	return err == nil && parsed.Scheme == "http" && net.ParseIP(parsed.Hostname()).IsLoopback() &&
		parsed.User == nil && parsed.Path == "" && parsed.RawQuery == "" && parsed.Fragment == ""
}

func main() {
	issuer := os.Getenv("RC_DEVICE_ISSUER")
	parsed, err := url.Parse(issuer)
	if err != nil || parsed.Scheme != "https" || parsed.Host == "" || parsed.Path != "" || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		log.Fatal("RC_DEVICE_ISSUER requires an own HTTPS origin")
	}
	key, err := loadKey(os.Getenv("RC_DEVICE_SIGNING_KEY"))
	if err != nil {
		log.Fatal(err)
	}
	s := &service{issuerURL: issuer, accountURL: os.Getenv("RC_ACCOUNT_URL"), linksURL: os.Getenv("RC_LINKS_URL"), linksKey: os.Getenv("RC_LINKS_KEY"), gameKey: os.Getenv("RC_DEVICE_GAME_KEY"), signing: key,
		client: &http.Client{Timeout: 5 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}, pending: make(map[string]*challenge), grants: make(map[string]grant), applications: make(map[string]bool)}
	publicAddr, privateAddr := os.Getenv("RC_DEVICE_LISTEN"), os.Getenv("RC_DEVICE_INTERNAL_LISTEN")
	privateHost, _, bindErr := net.SplitHostPort(privateAddr)
	if publicAddr == "" || bindErr != nil || !net.ParseIP(privateHost).IsLoopback() ||
		!loopbackHTTPOrigin(s.accountURL) || !loopbackHTTPOrigin(s.linksURL) || len(s.gameKey) < 32 || len(s.linksKey) < 32 || s.gameKey == s.linksKey {
		log.Fatal("native lab configuration is incomplete")
	}
	for _, entry := range strings.Split(os.Getenv("RC_DEVICE_APPLICATIONS"), ",") {
		entry = strings.ToLower(strings.TrimSpace(entry))
		if entry == "" {
			continue
		}
		if !applicationIDPattern.MatchString(entry) {
			log.Fatal("invalid configured RoomCircuit application")
		}
		s.applications[entry] = true
	}
	s.authority, err = coreAuthority(os.Getenv("RC_ACCOUNT_CORE_GRPC"), os.Getenv("RC_ACCOUNT_CORE_KEY"))
	if err != nil {
		log.Fatal(err)
	}
	realm := os.Getenv("RC_ACCOUNT_REALM")
	if realm != "" && realm != "email" && realm != "username" {
		log.Fatal("unknown account realm")
	}
	if realm == "username" {
		if err := s.validateUsernameRealm(); err != nil {
			log.Fatal("username realm verification failed")
		}
		s.usernameRealm = true
		s.authority.usernameRealm = true
	}
	s.durable, err = openDurableGrants(os.Getenv("RC_DEVICE_GRANT_STATE"))
	if err != nil {
		log.Fatal(err)
	}
	if err := s.syncRevocations(context.Background()); err != nil {
		log.Printf("initial account event catch-up incomplete: %v", err)
	}
	public, private := http.NewServeMux(), http.NewServeMux()
	health := func(w http.ResponseWriter, _ *http.Request) { response(w, 200, map[string]string{"status": "ok"}) }
	public.HandleFunc("GET /healthz", health)
	private.HandleFunc("GET /healthz", health)
	if s.usernameRealm {
		public.HandleFunc("GET /register", s.usernamePage)
		public.HandleFunc("POST /register", s.usernameRegister)
	}
	public.HandleFunc("POST /v1/device/authorize", s.authorize)
	public.HandleFunc("GET /device", s.browser)
	public.HandleFunc("POST /device", s.approve)
	public.HandleFunc("POST /v1/device/token", s.token)
	public.HandleFunc("POST /v1/device/refresh", s.renewal)
	public.HandleFunc("POST /v1/device/logout", s.renewal)
	private.HandleFunc("POST /internal/switch/identity", s.gameIdentity)
	private.HandleFunc("POST /internal/grants/check", s.revalidateGrant)
	private.HandleFunc("POST /internal/account/profile", s.accountProfile)
	instance := func(addr string, handler http.Handler) *http.Server {
		return &http.Server{Addr: addr, Handler: handler, ReadHeaderTimeout: 4 * time.Second, ReadTimeout: 8 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 8192}
	}
	go func() { log.Fatal(instance(privateAddr, private).ListenAndServe()) }()
	log.Fatal(instance(publicAddr, public).ListenAndServeTLS(os.Getenv("RC_DEVICE_CERT"), os.Getenv("RC_DEVICE_TLS_KEY")))
}
