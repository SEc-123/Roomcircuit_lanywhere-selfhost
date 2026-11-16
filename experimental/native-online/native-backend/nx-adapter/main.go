package main

import (
	"bytes"
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"log"
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

const (
	loginTTL    = 5 * time.Minute
	deviceLimit = 128
)

// A player is read from the independently approved OpenPak account binding.
type player struct {
	PID         uint64 `json:"pid"`
	User        string `json:"baas_user_id"`
	Account     string `json:"account_id"`
	Nickname    string `json:"nickname"`
	Country     string `json:"country"`
	Application string `json:"application_id"`
	Friends     []any  `json:"friends"`
	GrantID     string `json:"grant_id,omitempty"`
}

type device struct {
	password     [32]byte
	owner        player
	grant        string
	enrollment   deviceKey
	provisional  string
	registeredAt int64
	createdAt    int64
	linkedAt     int64
}

type session struct {
	deviceID    string
	account     string
	grant       string
	validUntil  time.Time
	forGame     bool
	provisional bool
}

type proof struct {
	Issuer        string           `json:"iss"`
	Audience      string           `json:"aud"`
	Subject       string           `json:"sub"`
	Purpose       string           `json:"typ"`
	Session       string           `json:"jti"`
	Expires       int64            `json:"exp"`
	Issued        int64            `json:"iat,omitempty"`
	DeviceBinding string           `json:"bs:did,omitempty"`
	GrantType     int              `json:"bs:grt,omitempty"`
	GrantState    []int64          `json:"bs:sts,omitempty"`
	Application   *baasApplication `json:"nintendo,omitempty"`
}

type adapter struct {
	issuer      string
	application string
	grantURL    string
	grantKey    string
	gameKey     string
	rsaKey      *rsa.PrivateKey
	store       *deviceStore
	bootstrap   *deviceBootstrap
	na          *naIssuer
	baas        *nativeBAAS
	baasKeys    *baasIssuerKeys
	aauth       *nativeAAUTH
	client      *http.Client
	mu          sync.Mutex
	devices     map[string]device
	sessions    map[string]session
	admitted    map[uint64]gameAdmission
}

func randomHex(count int) string {
	value := make([]byte, count)
	if _, err := rand.Read(value); err != nil {
		panic(err)
	}
	return hex.EncodeToString(value)
}

func respond(w http.ResponseWriter, code int, value any) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(value)
}

func deny(w http.ResponseWriter, status int, code string) {
	respond(w, status, map[string]string{"error": code})
}

func bearer(r *http.Request) string {
	value := r.Header.Get("Authorization")
	if !strings.HasPrefix(value, "Bearer ") {
		return ""
	}
	return strings.TrimSpace(strings.TrimPrefix(value, "Bearer "))
}

func (a *adapter) checkGrant(ctx context.Context, token string) (player, int) {
	if token == "" || len(token) > 2048 {
		return player{}, 401
	}
	return a.grantRequest(ctx, "/internal/switch/identity", map[string]string{"id_token": token})
}

func (a *adapter) grantRequest(ctx context.Context, path string, claims map[string]string) (player, int) {
	var who player
	claims["application_id"] = a.application
	data, _ := json.Marshal(claims)
	req, err := http.NewRequestWithContext(ctx, "POST", a.grantURL+path, bytes.NewReader(data))
	if err != nil {
		return who, 503
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Internal-Key", a.grantKey)
	res, err := a.client.Do(req)
	if err != nil {
		return who, 503
	}
	defer res.Body.Close()
	switch res.StatusCode {
	case 200:
		if json.NewDecoder(io.LimitReader(res.Body, 8192)).Decode(&who) != nil || who.PID == 0 || who.User == "" || who.Account == "" || who.Application != a.application {
			return player{}, 503
		}
		return who, 200
	case 401, 403, 404:
		return who, res.StatusCode
	default:
		return who, 503
	}
}

func (a *adapter) userInput(who player, id string, created, linked int64, hasAccount bool) (wire.BaaSUserInput, error) {
	input := wire.BaaSUserInput{ID: who.User, DeviceBindingID: id, Country: who.Country, CDNOrigin: a.issuer,
		CreatedAt: created, UpdatedAt: created, DeviceIDs: []string{id},
		Profile: wire.BaaSProfile{Nickname: who.Nickname, NicknameUpdatedAt: created}}
	if a.baas != nil {
		input.CDNOrigin = a.baas.cdn
		if hasAccount || who.Account == "" {
			record, err := a.baas.profiles.read(who.User)
			if err != nil {
				return wire.BaaSUserInput{}, err
			}
			if record.CreatedAt == 0 {
				record.CreatedAt, record.UpdatedAt = created, created
			}
			input = a.baas.snapshot(who, record)
			if !slices.Contains(input.DeviceIDs, id) {
				input.DeviceIDs = append(input.DeviceIDs, id)
			}
		}
	}
	if hasAccount && (input.Account == nil || input.Account.CreatedAt == 0) {
		input.Account = &wire.AccountLink{ID: naSubject(who), CreatedAt: linked, UpdatedAt: linked}
	}
	return input, nil
}

func (a *adapter) registerDevice(w http.ResponseWriter, r *http.Request) {
	token := bearer(r)
	enrollment, native := a.bootstrap.enrolledToken(token)
	var who player
	if !native {
		if a.bootstrap != nil {
			a.baasFailure(w, r, 401, "invalid_token")
			return
		}
		var status int
		who, status = a.federationGrant(r.Context(), token, enrollment)
		if status != 200 {
			a.baasFailure(w, r, status, "invalid_token")
			return
		}
	}
	now := time.Now()
	id, password := randomHex(16), randomHex(32)
	item := device{password: sha256.Sum256([]byte(password)), owner: who, createdAt: now.Unix()}
	if native {
		id = randomHex(8)
		item.enrollment = enrollment
		item.provisional = randomHex(8)
		item.registeredAt = now.Unix()
		who.User = item.provisional
	}
	input, err := a.userInput(who, id, item.createdAt, 0, false)
	if err != nil {
		a.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	registration, err := wire.BuildBaaSRegistration(input, id, password)
	if err != nil {
		a.baasFailure(w, r, 409, "complete_account_profile")
		return
	}
	a.mu.Lock()
	active, enrolled := 0, 0
	for _, existing := range a.devices {
		if !existing.registrationExpired(now) {
			active++
			if native && existing.enrollment == enrollment {
				enrolled++
			}
			if native && (existing.provisional == who.User || existing.owner.User == who.User) {
				a.mu.Unlock()
				a.baasFailure(w, r, 503, "under_maintenance")
				return
			}
		}
	}
	if active >= deviceLimit || enrolled >= enrolledUserLimit {
		a.mu.Unlock()
		a.baasFailure(w, r, 429, "device_limit")
		return
	}
	if _, exists := a.devices[id]; exists {
		a.mu.Unlock()
		a.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	if err := a.saveDevice(id, item); err != nil {
		a.mu.Unlock()
		log.Printf("save new device failed: %v", err)
		a.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	a.mu.Unlock()
	w.Header().Set("Location", "/1.0.0/users/"+who.User)
	respond(w, 201, registration)
}

var jwtEncoding = base64.RawURLEncoding
var jwtHeader = jwtEncoding.EncodeToString([]byte(`{"alg":"RS256","typ":"JWT"}`))

func (a *adapter) signed(c proof) (string, error) {
	return a.signProof(jwtHeader, c)
}

func (a *adapter) signProof(header string, c proof) (string, error) {
	body, err := json.Marshal(c)
	if err != nil {
		return "", err
	}
	text := header + "." + jwtEncoding.EncodeToString(body)
	digest := sha256.Sum256([]byte(text))
	sig, err := rsa.SignPKCS1v15(rand.Reader, a.rsaKey, crypto.SHA256, digest[:])
	if err != nil {
		return "", err
	}
	return text + "." + jwtEncoding.EncodeToString(sig), nil
}

func (a *adapter) authenticated(ctx context.Context, token string, purpose string) (player, int) {
	return a.authorizeBAAS(ctx, token, purpose, false)
}

func (a *adapter) profilePrincipal(ctx context.Context, token string) (player, int) {
	return a.authorizeBAAS(ctx, token, "token", true)
}

func (a *adapter) authorizeBAAS(ctx context.Context, token, purpose string, allowProvisional bool) (player, int) {
	var who player
	header, audience := jwtHeader, a.application
	if a.baasKeys != nil {
		header, audience = a.baasKeys.header(purpose), a.baasKeys.audience
	}
	parts := strings.Split(token, ".")
	if len(token) > 3072 || len(parts) != 3 || parts[0] != header {
		return who, 401
	}
	sig, err := jwtEncoding.DecodeString(parts[2])
	if err != nil {
		return who, 401
	}
	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	if rsa.VerifyPKCS1v15(&a.rsaKey.PublicKey, crypto.SHA256, digest[:], sig) != nil {
		return who, 401
	}
	body, err := jwtEncoding.DecodeString(parts[1])
	var c proof
	now := time.Now()
	if err != nil || json.Unmarshal(body, &c) != nil || c.Issuer != a.issuer || c.Audience != audience || c.Purpose != purpose || c.Expires <= now.Unix() {
		return who, 401
	}
	if a.baasKeys != nil && (c.Issued <= 0 || c.Issued > now.Unix() || (purpose == "token" && c.GrantType != 2)) {
		return player{}, 401
	}
	a.mu.Lock()
	s := a.sessions[c.Session]
	d := a.devices[s.deviceID]
	a.mu.Unlock()
	if s.provisional {
		if !allowProvisional || !now.Before(s.validUntil) || d.registrationExpired(now) || d.provisional == "" || d.provisional != c.Subject {
			return player{}, 404
		}
		local := player{User: d.provisional, Application: a.application}
		if c.DeviceBinding != baasBindingID(local) {
			return player{}, 401
		}
		return local, 200
	}
	if s.deviceID == "" || !now.Before(s.validUntil) || d.owner.User != c.Subject || d.owner.Account != s.account || d.grant != s.grant {
		return who, 404
	}
	if a.baasKeys != nil {
		if c.DeviceBinding != baasBindingID(d.owner) {
			return player{}, 401
		}
		if purpose == "id_token" && d.enrollment != (deviceKey{}) && !a.baasKeys.validTitleTicket(c, d.enrollment, a.application) {
			return player{}, 403
		}
	}
	who, status := a.revalidateGrant(ctx, s.grant)
	if status != 200 {
		return player{}, status
	}
	if who.Account != s.account || who.PID != d.owner.PID || !a.acceptSession(c.Session, s, d.owner, purpose == "id_token") {
		return player{}, 403
	}
	who.User = d.owner.User
	return who, 200
}

func (a *adapter) acceptSession(id string, expected session, owner player, admit bool) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	current, exists := a.sessions[id]
	item := a.devices[expected.deviceID]
	now := time.Now()
	valid := exists && current == expected && now.Before(current.validUntil) && item.grant == expected.grant && (!admit || current.forGame) &&
		item.owner.Application == a.application && item.owner.PID == owner.PID && item.owner.User == owner.User && item.owner.Account == owner.Account
	if valid && admit {
		a.admitted[owner.PID] = gameAdmission{deviceID: expected.deviceID, grant: expected.grant, expires: now.Add(admissionTTL)}
	}
	return valid
}

func (a *adapter) deviceSession(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil {
		a.baasFailure(w, r, 400, "invalid_params")
		return
	}
	id, password := r.PostForm.Get("id"), r.PostForm.Get("password")
	hash := sha256.Sum256([]byte(password))
	a.mu.Lock()
	entry, ok := a.devices[id]
	a.mu.Unlock()
	if !ok || subtle.ConstantTimeCompare(entry.password[:], hash[:]) != 1 {
		a.baasFailure(w, r, 400, "invalid_device_account")
		return
	}
	selected := entry.grant
	federation := r.URL.Path == "/1.0.0/federation"
	gameAuthorized := entry.enrollment == (deviceKey{}) && a.bootstrap == nil
	var title *baasApplication
	if entry.enrollment != (deviceKey{}) {
		key, approved := a.bootstrap.enrolledToken(bearer(r))
		if !approved || key != entry.enrollment || entry.registrationExpired(time.Now()) {
			a.baasFailure(w, r, 401, "invalid_token")
			return
		}
		if appProof := r.PostForm.Get("appAuthNToken"); appProof != "" {
			var validTitle bool
			title, validTitle = a.baasTitleProof(appProof, entry.enrollment)
			if !validTitle {
				a.baasFailure(w, r, 400, "invalid_ndas_app_authn_token")
				return
			}
			gameAuthorized = true
		}
	}
	unlinked := entry.provisional != ""
	provisionalLogin := unlinked && !federation
	if provisionalLogin && title != nil {
		a.baasFailure(w, r, 403, "membership_required")
		return
	}
	if federation {
		idp, supplied := r.PostForm["idp"]
		if (supplied && (len(idp) != 1 || idp[0] != "nintendoAccount")) || (!supplied && entry.enrollment != (deviceKey{})) {
			a.baasFailure(w, r, 400, "invalid_idp")
			return
		}
		selected = r.PostForm.Get("idToken")
	}
	if selected == "" && !provisionalLogin {
		if federation {
			a.baasFailure(w, r, 400, "invalid_idp_account")
		} else {
			a.baasFailure(w, r, 403, "membership_required")
		}
		return
	}
	var who player
	var status int
	switch {
	case provisionalLogin:
		who, status = player{User: entry.provisional, Application: a.application}, 200
	case federation:
		// Only a verified issuer token may introduce a new durable reference.
		who, status = a.federationGrant(r.Context(), selected, entry.enrollment)
		if status == 200 && who.GrantID != "" {
			selected = durableGrantPrefix + who.GrantID
		}
	default:
		who, status = a.revalidateGrant(r.Context(), selected)
	}
	if status != 200 {
		switch {
		case status == http.StatusServiceUnavailable:
			a.baasFailure(w, r, status, "under_maintenance")
		case federation && (status == 401 || status == 404):
			a.baasFailure(w, r, 400, "invalid_idp_account")
		default:
			a.baasFailure(w, r, status, "membership_required")
		}
		return
	}
	if !unlinked && (who.Account != entry.owner.Account || who.PID != entry.owner.PID) {
		a.baasFailure(w, r, 403, "insufficient_scope")
		return
	}
	if unlinked {
		who.User = entry.provisional
	} else {
		who.User = entry.owner.User
	}
	now := time.Now()
	validUntil := now.Add(loginTTL)
	if provisionalLogin {
		if registrationExpires := time.Unix(entry.registeredAt, 0).Add(registrationTTL); registrationExpires.Before(validUntil) {
			validUntil = registrationExpires
		}
		if validUntil.Sub(now) < time.Second {
			a.baasFailure(w, r, 401, "invalid_token")
			return
		}
	}
	ids, credentials, err := a.baasCredentials(who, entry.enrollment, title, now, validUntil)
	if err != nil {
		a.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	linkedAt := entry.linkedAt
	if linkedAt == 0 && federation {
		linkedAt = time.Now().Unix()
	}
	input, err := a.userInput(who, id, entry.createdAt, linkedAt, !provisionalLogin)
	if err != nil {
		a.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	result, err := wire.BuildBaaSLogin(input, credentials)
	if err != nil {
		a.baasFailure(w, r, 409, "complete_account_profile")
		return
	}
	a.mu.Lock()
	live, exists := a.devices[id]
	if !exists || live.provisional != entry.provisional || live.owner.Account != entry.owner.Account ||
		(live.grant != selected && (!federation || live.grant != entry.grant)) {
		a.mu.Unlock()
		a.baasFailure(w, r, 409, "resource_already_exists")
		return
	}
	retire := make([]string, 0, 2)
	for key, value := range a.sessions {
		if !now.Before(value.validUntil) {
			delete(a.sessions, key)
		} else if federation && unlinked && value.deviceID == id && value.provisional {
			retire = append(retire, key)
		}
	}
	if len(a.sessions)-len(retire) > 254 {
		a.mu.Unlock()
		a.baasFailure(w, r, 429, "session_limit")
		return
	}
	if federation && (unlinked || live.grant != selected || live.linkedAt != linkedAt) {
		if unlinked {
			live.owner = who
			live.provisional = ""
			live.registeredAt = 0
		}
		live.grant = selected
		live.linkedAt = linkedAt
		if err := a.saveDevice(id, live); err != nil {
			a.mu.Unlock()
			log.Printf("save device association failed: %v", err)
			a.baasFailure(w, r, 503, "under_maintenance")
			return
		}
	}
	for _, jti := range retire {
		delete(a.sessions, jti)
	}
	for _, jti := range ids {
		a.sessions[jti] = session{deviceID: id, account: who.Account, grant: selected, validUntil: validUntil,
			forGame: gameAuthorized, provisional: provisionalLogin}
	}
	a.mu.Unlock()
	respond(w, 200, result)
}

func (a *adapter) gameIdentity(w http.ResponseWriter, r *http.Request) {
	if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Internal-Key")), []byte(a.gameKey)) != 1 {
		deny(w, 401, "invalid_internal_key")
		return
	}
	var input struct {
		IDToken     string `json:"id_token"`
		PID         uint64 `json:"pid"`
		Application string `json:"application_id"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&input) != nil || decoder.Decode(new(any)) != io.EOF || (input.IDToken == "") == (input.PID == 0) {
		deny(w, 400, "invalid_request")
		return
	}
	if input.Application != "" && input.Application != a.application {
		deny(w, 403, "wrong_application")
		return
	}
	var who player
	var status int
	if input.PID != 0 {
		who, status = a.connectedPlayer(r.Context(), input.PID)
	} else {
		who, status = a.authenticated(r.Context(), input.IDToken, "id_token")
	}
	if status != 200 {
		deny(w, status, "game_identity_unavailable")
		return
	}
	respond(w, 200, who)
}

func key(path string) (*rsa.PrivateKey, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return nil, fmt.Errorf("invalid independent issuer key")
	}
	decoded, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, err
	}
	value, ok := decoded.(*rsa.PrivateKey)
	if !ok || value.N.BitLen() < 2048 {
		return nil, fmt.Errorf("issuer needs an RSA-2048+ key")
	}
	return value, value.Validate()
}

func origin(raw string, scheme string) (*url.URL, bool) {
	parsed, err := url.Parse(raw)
	return parsed, err == nil && parsed.Scheme == scheme && parsed.Hostname() != "" &&
		parsed.User == nil && parsed.Path == "" && parsed.RawQuery == "" && parsed.Fragment == ""
}

func main() {
	issuer, broker := os.Getenv("NX_LAB_ISSUER"), os.Getenv("NX_LAB_GRANT_URL")
	rsaKey, err := key(os.Getenv("NX_LAB_SIGNING_KEY"))
	if err != nil {
		log.Fatal(err)
	}
	a := &adapter{issuer: issuer, application: os.Getenv("NX_LAB_APPLICATION"), grantURL: broker,
		grantKey: os.Getenv("NX_LAB_GRANT_KEY"), gameKey: os.Getenv("NX_LAB_GAME_KEY"), rsaKey: rsaKey,
		client:   &http.Client{Timeout: 5 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},
		sessions: make(map[string]session), admitted: make(map[uint64]gameAdmission)}
	_, issuerOK := origin(issuer, "https")
	brokerOrigin, brokerOK := origin(broker, "http")
	if !issuerOK || !brokerOK || !net.ParseIP(brokerOrigin.Hostname()).IsLoopback() || !regexp.MustCompile(`^[0-9a-f]{16}$`).MatchString(a.application) || len(a.grantKey) < 32 || len(a.gameKey) < 32 || a.grantKey == a.gameKey {
		log.Fatal("owned HTTPS issuer and private grant configuration are incomplete")
	}
	publicAddr, internalAddr := os.Getenv("NX_LAB_LISTEN"), os.Getenv("NX_LAB_INTERNAL")
	if _, port, err := net.SplitHostPort(publicAddr); err != nil || port == "" {
		log.Fatal("NX_LAB_LISTEN requires a TLS bind address and port")
	}
	if host, _, err := net.SplitHostPort(internalAddr); err != nil || !net.ParseIP(host).IsLoopback() {
		log.Fatal("internal callback must bind loopback")
	}
	a.store, a.devices, err = openDeviceStore(os.Getenv("NX_LAB_STATE"), os.Getenv("NX_LAB_STATE_KEY"), a.application)
	if err != nil {
		log.Fatal(err)
	}
	public, private := http.NewServeMux(), http.NewServeMux()
	if err := a.addNativeBootstrap(public); err != nil {
		log.Fatal(err)
	}
	public.HandleFunc("POST /1.0.0/users", a.registerDevice)
	public.HandleFunc("POST /1.0.0/login", a.deviceSession)
	public.HandleFunc("POST /1.0.0/federation", a.deviceSession)
	if err := a.oauth(public); err != nil {
		log.Fatal(err)
	}
	if a.bootstrap != nil && a.na == nil {
		log.Fatal("native account linking requires its owned OAuth configuration")
	}
	if err := a.addNativeBAASRoutes(public); err != nil {
		log.Fatal(err)
	}
	if err := a.addBAASCertificates(public); err != nil {
		log.Fatal(err)
	}
	public.HandleFunc("/", func(w http.ResponseWriter, _ *http.Request) { deny(w, 501, "not_supported_in_network_only_lab") })
	private.HandleFunc("POST /internal/switch/identity", a.gameIdentity)
	private.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { respond(w, 200, map[string]string{"status": "ok"}) })
	server := func(addr string, handler http.Handler) *http.Server {
		return &http.Server{Addr: addr, Handler: handler, ReadHeaderTimeout: 4 * time.Second, ReadTimeout: 8 * time.Second, WriteTimeout: 10 * time.Second, IdleTimeout: 30 * time.Second}
	}
	go func() { log.Fatal(server(internalAddr, private).ListenAndServe()) }()
	cert, err := tls.LoadX509KeyPair(os.Getenv("NX_LAB_CERT"), os.Getenv("NX_LAB_TLS_KEY"))
	if err != nil {
		log.Fatal(err)
	}
	publicServer := server(publicAddr, public)
	publicServer.TLSConfig = a.bootstrap.tlsConfig(cert)
	log.Fatal(publicServer.ListenAndServeTLS("", ""))
}
