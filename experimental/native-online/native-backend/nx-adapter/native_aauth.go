package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net/http"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"
)

type nativeAAUTH struct {
	a        *adapter
	clientID string
	redirect string
	issuer   string
	kid      string
	header   string
	mu       sync.Mutex
	issued   map[[32]byte]deviceProof
}

const applicationAuthTTL = 10 * time.Minute

var nativeVersion = regexp.MustCompile(`^[0-9a-fA-F]{1,8}$`)

func (a *adapter) addNativeAAUTH(mux *http.ServeMux) error {
	clientID := os.Getenv("NX_LAB_TITLE_OAUTH_CLIENT_ID")
	if clientID == "" {
		if a.bootstrap != nil && a.bootstrap.dcert != nil {
			mux.HandleFunc("GET /keys", a.ndasPublicKeys)
		}
		return nil
	}
	if a.bootstrap == nil || a.na == nil || !validNativeUserID(clientID) {
		return errors.New("native title authorization needs device bootstrap and a registered 16-hex OAuth client")
	}
	redirect := "npf" + clientID + "://auth"
	if registered := a.na.audiences[clientID]; registered == nil || registered.String() != redirect {
		return errors.New("native title authorization does not match the OAuth redirect registry")
	}
	keyOrigin := os.Getenv("NX_LAB_AAUTH_KEY_ORIGIN")
	if keyOrigin == "" {
		keyOrigin = a.issuer
	}
	if _, valid := origin(keyOrigin, "https"); !valid {
		return errors.New("native title signing keys require an owned HTTPS origin")
	}
	issuerOrigin := os.Getenv("NX_LAB_AAUTH_ORIGIN")
	if issuerOrigin == "" {
		issuerOrigin = a.issuer
	}
	issuer, valid := origin(issuerOrigin, "https")
	if !valid {
		return errors.New("native title issuer requires an owned HTTPS origin")
	}
	kid := ndasKeyID("roomcircuit-application-key-v1", &a.rsaKey.PublicKey)
	header, err := ndasHeader(keyOrigin, kid)
	if err != nil {
		return err
	}
	service := &nativeAAUTH{
		a: a, clientID: clientID, redirect: redirect, issuer: issuer.Hostname(), kid: kid, header: header,
		issued: make(map[[32]byte]deviceProof),
	}
	a.aauth = service
	mux.HandleFunc("POST /v5/application_auth_token", service.applicationToken)
	mux.HandleFunc("GET /keys", a.ndasPublicKeys)
	return nil
}

func (s *nativeAAUTH) issue(verified deviceProof, version string, now time.Time) (string, error) {
	// Use the independently enrolled key as the title request's continuity
	// proof. This token carries no retail purchase or account entitlement.
	identity := verified.identity
	if identity.id == "" {
		identity = nativeDevice(verified.key, nil, false)
	}
	binding := sha256.Sum256(append([]byte("roomcircuit-app-device-v1\x00"), verified.key[:]...))
	return signNDAS(s.a.rsaKey, s.header, map[string]any{
		"iss": s.issuer, "aud": s.clientID, "sub": s.a.application, "typ": "application_auth_token",
		"iat": now.Unix(), "exp": now.Add(applicationAuthTTL).Unix(), "jti": uuid4(),
		"dvc": map[string]string{"di": identity.id, "sn": identity.serial},
		"nintendo": map[string]any{
			"ai": s.a.application, "av": version, "at": now.Unix(), "edi": hex.EncodeToString(binding[:16]),
			"di": identity.id, "sn": identity.serial,
			"opp": "MEMBERSHIP_REQUIRED", "ph": "SYSTEM",
		},
	})
}

func (s *nativeAAUTH) applicationToken(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 8192)
	if r.ParseForm() != nil || len(r.PostForm["application_id"]) != 1 ||
		len(r.PostForm["application_version"]) != 1 || len(r.PostForm["device_auth_token"]) != 1 {
		ndasError(w, 400, "0118", "Invalid parameter in request.")
		return
	}
	application, version := strings.ToLower(r.PostForm.Get("application_id")), r.PostForm.Get("application_version")
	if application != s.a.application || !nativeVersion.MatchString(version) {
		ndasError(w, 403, "0106", "Unauthorized application.")
		return
	}
	credential := r.PostForm.Get("device_auth_token")
	verified, valid := s.a.bootstrap.deviceAssertion(credential)
	if !valid {
		if s.a.bootstrap.expiredTitleDeviceToken(credential, time.Now()) {
			ndasError(w, 401, "0103", "Device token expired.")
		} else {
			ndasError(w, 401, "0118", "Invalid device token.")
		}
		return
	}
	if verified.clientID != s.a.bootstrap.clientID {
		ndasError(w, 401, "0106", "Unauthorized device token for title authorization.")
		return
	}
	now := time.Now()
	s.mu.Lock()
	pruneProofs(s.issued, now)
	if len(s.issued) >= bootstrapLimit {
		s.mu.Unlock()
		ndasError(w, 429, "0112", "Application token service busy.")
		return
	}
	token, err := s.issue(verified, version, now)
	if err != nil {
		s.mu.Unlock()
		ndasError(w, 503, "0112", "Internal server error.")
		return
	}
	s.issued[sha256.Sum256([]byte(token))] = deviceProof{key: verified.key, expires: now.Add(applicationAuthTTL)}
	s.mu.Unlock()
	respond(w, 200, map[string]any{
		"application_auth_token": token, "expires_in": int(applicationAuthTTL / time.Second),
		"settings":           []map[string]string{{"nas:client_id": s.clientID, "nas:redirect_uri": s.redirect}},
		"online_play_policy": "MEMBERSHIP_REQUIRED", "policy_handler": "SYSTEM",
	})
}

func (a *adapter) verifyNativeAppToken(token string, enrollment deviceKey) bool {
	if a.aauth == nil || enrollment == (deviceKey{}) || len(token) == 0 || len(token) > 2048 {
		return false
	}
	s := a.aauth
	s.mu.Lock()
	defer s.mu.Unlock()
	pruneProofs(s.issued, time.Now())
	proof, exists := s.issued[sha256.Sum256([]byte(token))]
	return exists && proof.key == enrollment
}
