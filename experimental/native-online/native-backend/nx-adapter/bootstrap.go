package main

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
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
	challengeTTL      = 2 * time.Minute
	deviceProofTTL    = 5 * time.Minute
	bootstrapTTL      = 10 * time.Minute
	bootstrapLimit    = 256
	enrolledUserLimit = 8
	registrationTTL   = 24 * time.Hour
)

type deviceKey [32]byte

type deviceProof struct {
	key                 deviceKey
	expires             time.Time
	clientID            string
	identity            nativeDeviceIdentity
	baasToken           string
	challengeVersion    uint8
	challengeGeneration uint8
}

type deviceBootstrap struct {
	host, clientID string
	enrolled       map[deviceKey]struct{}
	adapter        *adapter
	dcert          *nativeDCERT

	mu         sync.Mutex
	challenges map[[32]byte]deviceProof
	assertions map[[32]byte]deviceProof
	tokens     map[[32]byte]deviceProof
}

func newDeviceBootstrap(a *adapter, host, clientID, pins string) (*deviceBootstrap, error) {
	if host == "" && clientID == "" && pins == "" {
		return nil, nil
	}
	if clientID == "" {
		clientID = hos19GameDevice
	}
	authority, err := url.Parse("https://" + host)
	if err != nil || host == "" || authority.Host != host || authority.User != nil ||
		authority.Path != "" || authority.RawQuery != "" || authority.Fragment != "" ||
		authority.Hostname() != host || net.ParseIP(host) != nil || !strings.Contains(host, ".") ||
		!regexp.MustCompile(`^[a-z0-9]([a-z0-9.-]*[a-z0-9])?$`).MatchString(host) ||
		!validNativeUserID(clientID) || clientID == hos19AccountApplet || clientID == hos19AccountAPI || len(pins) > 65*deviceLimit {
		return nil, errors.New("native bootstrap requires a dedicated DNS name, client ID and enrolled SPKI fingerprints")
	}
	if issuer, ok := origin(a.issuer, "https"); !ok || strings.EqualFold(issuer.Hostname(), host) {
		return nil, errors.New("device TLS and the account issuer need distinct hosts")
	}
	d := &deviceBootstrap{
		host: host, clientID: clientID, adapter: a,
		enrolled:   make(map[deviceKey]struct{}),
		challenges: make(map[[32]byte]deviceProof), assertions: make(map[[32]byte]deviceProof),
		tokens: make(map[[32]byte]deviceProof),
	}
	for pin := range strings.SplitSeq(pins, ",") {
		raw, err := hex.DecodeString(strings.TrimSpace(pin))
		if err != nil || len(raw) != 32 || deviceKey(raw) == (deviceKey{}) {
			return nil, errors.New("invalid enrolled device SPKI fingerprint")
		}
		d.enrolled[deviceKey(raw)] = struct{}{}
	}
	if len(d.enrolled) == 0 || len(d.enrolled) > deviceLimit {
		return nil, errors.New("native bootstrap needs a bounded enrollment list")
	}
	return d, nil
}

func (d *deviceBootstrap) tlsConfig(cert tls.Certificate) *tls.Config {
	base := &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{cert}}
	if d == nil {
		return base
	}
	// Only the device-auth host requests a client certificate. An enrolled
	// SPKI pin authorizes it inside this realm after TLS verifies possession.
	// Requests without that proof can still use the browser/OAuth host.
	device := base.Clone()
	device.ClientAuth = tls.RequestClientCert
	device.SessionTicketsDisabled = true
	base.GetConfigForClient = func(hello *tls.ClientHelloInfo) (*tls.Config, error) {
		if strings.EqualFold(hello.ServerName, d.host) {
			return device, nil
		}
		return nil, nil
	}
	return base
}

func (d device) registrationExpired(now time.Time) bool {
	return d.provisional != "" && !now.Before(time.Unix(d.registeredAt, 0).Add(registrationTTL))
}

func (d *deviceBootstrap) key(r *http.Request, now time.Time) (deviceKey, bool) {
	state := r.TLS
	if state == nil || !state.HandshakeComplete || !strings.EqualFold(state.ServerName, d.host) ||
		len(state.PeerCertificates) == 0 {
		return deviceKey{}, false
	}
	leaf := state.PeerCertificates[0]
	if now.Before(leaf.NotBefore) || !now.Before(leaf.NotAfter) {
		return deviceKey{}, false
	}
	id := deviceKey(sha256.Sum256(leaf.RawSubjectPublicKeyInfo))
	_, enrolled := d.enrolled[id]
	return id, enrolled
}

func pruneProofs(entries map[[32]byte]deviceProof, now time.Time) {
	for key, item := range entries {
		if !now.Before(item.expires) {
			delete(entries, key)
		}
	}
}

func (d *deviceBootstrap) challenge(w http.ResponseWriter, r *http.Request) {
	now := time.Now()
	key, enrolled := d.key(r, now)
	if !enrolled {
		ndasError(w, 401, "0004", "Unauthorized device.")
		return
	}
	challenge := randomBase64(33)
	if d.keepChallenge(w, key, challenge, now.Add(challengeTTL), 8, 0) {
		respond(w, 200, map[string]string{"challenge": challenge, "data": randomBase64(16)})
	}
}

func (d *deviceBootstrap) keepChallenge(w http.ResponseWriter, key deviceKey, challenge string, expires time.Time, version, generation uint8) bool {
	d.mu.Lock()
	pruneProofs(d.challenges, time.Now())
	if len(d.challenges) >= bootstrapLimit {
		d.mu.Unlock()
		ndasError(w, 429, "0009", "Device challenge service busy.")
		return false
	}
	d.challenges[sha256.Sum256([]byte(challenge))] = deviceProof{
		key: key, expires: expires, challengeVersion: version, challengeGeneration: generation,
	}
	d.mu.Unlock()
	return true
}

func randomBase64(size int) string {
	return base64.StdEncoding.EncodeToString(randomBytes(size))
}

func randomBytes(size int) []byte {
	data := make([]byte, size)
	if _, err := rand.Read(data); err != nil {
		panic(err)
	}
	return data
}

func (d *deviceBootstrap) deviceAuth(w http.ResponseWriter, r *http.Request) {
	now := time.Now()
	key, enrolled := d.key(r, now)
	if !enrolled {
		ndasError(w, 401, "0004", "Unauthorized device.")
		return
	}
	var request struct {
		Challenge     string `json:"challenge"`
		IST           bool   `json:"ist"`
		TokenRequests []struct {
			ClientID string `json:"client_id"`
		} `json:"token_requests"`
	}
	r.Body = http.MaxBytesReader(w, r.Body, 8192)
	decoder := json.NewDecoder(r.Body)
	if decoder.Decode(&request) != nil || !errors.Is(decoder.Decode(new(any)), io.EOF) ||
		len(request.Challenge) != 44 || len(request.TokenRequests) == 0 || len(request.TokenRequests) > 16 {
		ndasError(w, 400, "0014", "Invalid parameter in request.")
		return
	}
	found := false
	for _, requested := range request.TokenRequests {
		if requested.ClientID == d.clientID {
			if found {
				ndasError(w, 400, "0014", "Invalid parameter in request.")
				return
			}
			found = true
		}
	}
	if !found {
		ndasError(w, 400, "0011", "Invalid client.")
		return
	}
	d.completeDeviceChallenge(w, key, nativeDevice(key, r.TLS.PeerCertificates[0], request.IST), d.clientID, request.Challenge, 8, 0)
}

func (d *deviceBootstrap) completeDeviceChallenge(w http.ResponseWriter, key deviceKey, identity nativeDeviceIdentity, clientID, value string, version, generation uint8) {
	digest := sha256.Sum256([]byte(value))
	d.mu.Lock()
	now := time.Now()
	pruneProofs(d.challenges, now)
	pruneProofs(d.assertions, now)
	challenge, ok := d.challenges[digest]
	if !ok || challenge.key != key || challenge.challengeVersion != version || challenge.challengeGeneration != generation {
		d.mu.Unlock()
		ndasError(w, 401, "0004", "Unauthorized device challenge.")
		return
	}
	if len(d.assertions) >= bootstrapLimit {
		d.mu.Unlock()
		ndasError(w, 429, "0009", "Device token service busy.")
		return
	}
	expires := time.Unix(now.Add(deviceProofTTL).Unix(), 0)
	token, err := d.signDevice(identity, clientID, now, expires)
	if err != nil {
		d.mu.Unlock()
		ndasError(w, 503, "0009", "Internal server error.")
		return
	}
	delete(d.challenges, digest)
	d.assertions[sha256.Sum256([]byte(token))] = deviceProof{
		key: key, expires: expires, clientID: clientID, identity: identity,
	}
	d.mu.Unlock()
	if version == 7 {
		respond(w, 200, map[string]any{"device_auth_token": token, "expires_in": int(deviceProofTTL / time.Second)})
		return
	}
	respond(w, 200, map[string]any{"results": []map[string]any{{
		"client_id": clientID, "device_auth_token": token, "expires_in": int(deviceProofTTL / time.Second),
	}}})
}

func (d *deviceBootstrap) applicationToken(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil || len(r.PostForm["assertion"]) != 1 || len(r.PostForm["grantType"]) != 1 ||
		r.PostForm.Get("grantType") != "public_client" {
		d.adapter.baasFailure(w, r, 400, "invalid_params")
		return
	}
	assertion := r.PostForm.Get("assertion")
	if len(assertion) == 0 || len(assertion) > 2048 {
		d.adapter.baasFailure(w, r, 400, "invalid_params")
		return
	}
	digest := sha256.Sum256([]byte(assertion))
	d.mu.Lock()
	now := time.Now()
	pruneProofs(d.assertions, now)
	pruneProofs(d.tokens, now)
	validated, ok := d.assertions[digest]
	if !ok {
		d.mu.Unlock()
		d.adapter.baasFailure(w, r, 401, "invalid_token")
		return
	}
	if validated.clientID != d.clientID {
		d.mu.Unlock()
		d.adapter.baasFailure(w, r, 403, "insufficient_scope")
		return
	}
	// A dropped response can be retried without issuing another token.
	if cached, exists := d.tokens[sha256.Sum256([]byte(validated.baasToken))]; exists {
		d.mu.Unlock()
		respond(w, 200, map[string]any{"accessToken": validated.baasToken, "expiresIn": cached.expires.Unix() - now.Unix(), "tokenType": "Bearer"})
		return
	}
	if len(d.tokens) >= bootstrapLimit {
		d.mu.Unlock()
		d.adapter.baasFailure(w, r, 429, "under_maintenance")
		return
	}
	expires := time.Unix(now.Add(bootstrapTTL).Unix(), 0)
	token, err := d.signAnonymous(validated, now, expires)
	if err != nil {
		d.mu.Unlock()
		d.adapter.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	validated.baasToken = token
	d.assertions[digest] = validated
	d.tokens[sha256.Sum256([]byte(token))] = deviceProof{key: validated.key, expires: expires}
	d.mu.Unlock()
	respond(w, 200, map[string]any{"accessToken": token, "expiresIn": int(bootstrapTTL / time.Second), "tokenType": "Bearer"})
}

func (d *deviceBootstrap) enrolledToken(token string) (deviceKey, bool) {
	if d == nil || len(token) == 0 || len(token) > 2048 {
		return deviceKey{}, false
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	pruneProofs(d.tokens, time.Now())
	issued, ok := d.tokens[sha256.Sum256([]byte(token))]
	return issued.key, ok
}

func (d *deviceBootstrap) deviceAuthorization(token string) (deviceKey, bool) {
	issued, ok := d.deviceAssertion(token)
	return issued.key, ok && issued.clientID == d.clientID
}

func (d *deviceBootstrap) accountDeviceAuthorization(token string) (deviceKey, bool) {
	issued, ok := d.deviceAssertion(token)
	return issued.key, ok && (issued.clientID == hos19AccountApplet || issued.clientID == hos19AccountAPI)
}

func (d *deviceBootstrap) deviceAssertion(token string) (deviceProof, bool) {
	if d == nil || len(token) == 0 || len(token) > 2048 {
		return deviceProof{}, false
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	pruneProofs(d.assertions, time.Now())
	issued, ok := d.assertions[sha256.Sum256([]byte(token))]
	return issued, ok
}

func (a *adapter) addNativeBootstrap(mux *http.ServeMux) error {
	d, err := newDeviceBootstrap(a, os.Getenv("NX_LAB_DEVICE_HOST"), os.Getenv("NX_LAB_DEVICE_CLIENT_ID"), os.Getenv("NX_LAB_ENROLLED_SPKI"))
	if err != nil {
		return err
	}
	a.bootstrap = d
	if d != nil {
		if err := d.loadDCERT(); err != nil {
			return err
		}
		mux.HandleFunc("POST /v7/challenge", d.challengeV7)
		mux.HandleFunc("POST /v7/device_auth_token", d.deviceAuthV7)
		mux.HandleFunc("POST /v8/challenge", d.challenge)
		mux.HandleFunc("POST /v8/device_auth_tokens", d.deviceAuth)
		mux.HandleFunc("POST /1.0.0/application/token", d.applicationToken)
		mux.HandleFunc("GET /2.0.0/agreements", func(w http.ResponseWriter, _ *http.Request) {
			respond(w, 200, map[string]any{"agreements": []any{}})
		})
		mux.HandleFunc("POST /2.0.0/agreements", func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Cache-Control", "no-store")
			w.WriteHeader(http.StatusNoContent)
		})
	}
	return nil
}
