package main

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math/big"
	"net/http"
	"os"
	"strings"
	"time"

	"roomcircuit.local/nx-adapter/wire"
)

type baasApplication struct {
	Title      string `json:"ai"`
	Version    string `json:"av"`
	Issued     int64  `json:"at"`
	Device     string `json:"edi"`
	TicketHash string `json:"tbh"`
	Nonce      string `json:"tbn"`
}

type baasIssuerKeys struct {
	audience     string
	idHeader     string
	accessHeader string
	deviceSeal   cipher.AEAD
	ticketKey    [32]byte
}

func (b *baasIssuerKeys) header(purpose string) string {
	if purpose == "id_token" {
		return b.idHeader
	}
	return b.accessHeader
}

func (a *adapter) addBAASCertificates(mux *http.ServeMux) error {
	keyOrigin := os.Getenv("NX_LAB_BAAS_KEY_ORIGIN")
	if keyOrigin == "" {
		keyOrigin = a.issuer
	}
	parsed, valid := origin(keyOrigin, "https")
	if !valid {
		return errors.New("BAAS signing keys require an owned HTTPS origin")
	}
	audience := os.Getenv("NX_LAB_BAAS_AUDIENCE")
	if audience == "" {
		audience = a.application
	}
	if !validNativeUserID(audience) {
		return errors.New("BAAS audience requires an explicitly configured 16-hex client ID")
	}
	idPath := "/1.0.0/certificates"
	keyPattern := "GET "
	if a.na != nil {
		naOrigin, _ := origin(a.na.origin, "https")
		if strings.EqualFold(naOrigin.Hostname(), parsed.Hostname()) {
			if a.bootstrap != nil {
				return errors.New("native NA and BAAS key origins require separate hostnames")
			}
			idPath = "/1.0.0/baas_certificates"
		} else {
			keyPattern += parsed.Hostname()
		}
	}
	const accessPath = "/1.0.0/internal_certificates"
	pub := x509.MarshalPKCS1PublicKey(&a.rsaKey.PublicKey)
	newHeader := func(purpose, path string) (kid, encoded string) {
		fingerprint := sha256.Sum256(append([]byte(purpose+"\x00"), pub...))
		kid = hex.EncodeToString(fingerprint[:16])
		body, _ := json.Marshal(map[string]string{"alg": "RS256", "typ": "JWT", "kid": kid, "jku": keyOrigin + path})
		return kid, jwtEncoding.EncodeToString(body)
	}
	idKid, idHeader := newHeader("roomcircuit-baas-id-v1", idPath)
	accessKid, accessHeader := newHeader("roomcircuit-baas-access-v1", accessPath)
	sealKey := sha256.Sum256([]byte("roomcircuit-baas-device-v1\x00" + a.grantKey))
	block, err := aes.NewCipher(sealKey[:])
	if err != nil {
		return err
	}
	seal, err := cipher.NewGCM(block)
	if err != nil {
		return err
	}
	a.baasKeys = &baasIssuerKeys{audience: audience, idHeader: idHeader, accessHeader: accessHeader,
		deviceSeal: seal, ticketKey: sha256.Sum256([]byte("roomcircuit-baas-ticket-v1\x00" + a.grantKey))}
	mux.HandleFunc(keyPattern+idPath, baasPublicKey(&a.rsaKey.PublicKey, idKid))
	mux.HandleFunc(keyPattern+accessPath, baasPublicKey(&a.rsaKey.PublicKey, accessKid))
	return nil
}

func baasPublicKey(key *rsa.PublicKey, kid string) http.HandlerFunc {
	set := map[string]any{"keys": []map[string]string{{"kty": "RSA", "alg": "RS256", "use": "sig", "kid": kid,
		"n": jwtEncoding.EncodeToString(key.N.Bytes()), "e": jwtEncoding.EncodeToString(big.NewInt(int64(key.E)).Bytes())}}}
	return func(w http.ResponseWriter, _ *http.Request) { respond(w, 200, set) }
}

func (a *adapter) baasTitleProof(token string, enrollment deviceKey) (*baasApplication, bool) {
	if !a.verifyNativeAppToken(token, enrollment) {
		return nil, false
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, false
	}
	body, err := jwtEncoding.DecodeString(parts[1])
	var authorized struct {
		Application *baasApplication `json:"nintendo"`
	}
	if err != nil || json.Unmarshal(body, &authorized) != nil || authorized.Application == nil {
		return nil, false
	}
	app := authorized.Application
	return app, app.Title == a.application && nativeVersion.MatchString(app.Version) && app.Issued > 0 && app.Issued <= time.Now().Unix()
}

func (a *adapter) baasCredentials(who player, enrollment deviceKey, title *baasApplication, now, validUntil time.Time) ([2]string, wire.BaaSLoginCredentials, error) {
	ids := [2]string{uuid4(), uuid4()}
	keys := a.baasKeys
	if keys == nil {
		return ids, wire.BaaSLoginCredentials{}, errors.New("BAAS issuer keys unavailable")
	}
	c := proof{Issuer: a.issuer, Audience: keys.audience, Subject: who.User, Issued: now.Unix(), Expires: validUntil.Unix(),
		Session: ids[0], Purpose: "id_token", DeviceBinding: baasBindingID(who), Application: title}
	if title != nil {
		attested, err := keys.titleTicket(c, enrollment)
		if err != nil {
			return ids, wire.BaaSLoginCredentials{}, err
		}
		c.Application = &attested
	}
	id, err := a.signProof(keys.idHeader, c)
	if err != nil {
		return ids, wire.BaaSLoginCredentials{}, err
	}
	c.Purpose, c.Session, c.Application, c.GrantType = "token", ids[1], nil, 2
	// The wire user-grant status vector is metadata for the client. Every
	// service operation still checks the independent account, device and grant.
	c.GrantState = []int64{10415858080830506, 272640, 9, 0, 0, 19316357715722240, 16}
	access, err := a.signProof(keys.accessHeader, c)
	return ids, wire.BaaSLoginCredentials{IDToken: id, AccessToken: access, ExpiresIn: int(validUntil.Sub(now) / time.Second)}, err
}

func (b *baasIssuerKeys) titleTicket(c proof, device deviceKey) (baasApplication, error) {
	app := *c.Application
	aad := []byte(strings.Join([]string{c.Session, c.Subject, app.Title, app.Version}, "\x00"))
	deviceNonce := make([]byte, b.deviceSeal.NonceSize())
	if _, err := rand.Read(deviceNonce); err != nil {
		return app, err
	}
	ticketNonce := make([]byte, 16)
	if _, err := rand.Read(ticketNonce); err != nil {
		return app, err
	}
	app.Device = base64.StdEncoding.EncodeToString(b.deviceSeal.Seal(deviceNonce, deviceNonce, device[:], aad))
	app.Nonce = base64.StdEncoding.EncodeToString(ticketNonce)
	app.TicketHash = base64.StdEncoding.EncodeToString(b.ticketHash(aad, ticketNonce, device))
	return app, nil
}

func (b *baasIssuerKeys) ticketHash(aad, nonce []byte, device deviceKey) []byte {
	mac := hmac.New(sha256.New, b.ticketKey[:])
	mac.Write(aad)
	mac.Write(nonce)
	mac.Write(device[:])
	return mac.Sum(nil)
}

func (b *baasIssuerKeys) validTitleTicket(c proof, device deviceKey, title string) bool {
	app := c.Application
	if app == nil || app.Title != title || !nativeVersion.MatchString(app.Version) || app.Issued <= 0 || app.Issued > c.Issued {
		return false
	}
	sealed, sealErr := base64.StdEncoding.DecodeString(app.Device)
	nonce, nonceErr := base64.StdEncoding.DecodeString(app.Nonce)
	ticket, ticketErr := base64.StdEncoding.DecodeString(app.TicketHash)
	size := b.deviceSeal.NonceSize()
	if sealErr != nil || nonceErr != nil || ticketErr != nil || len(nonce) != 16 || len(sealed) != size+len(device)+b.deviceSeal.Overhead() {
		return false
	}
	aad := []byte(strings.Join([]string{c.Session, c.Subject, app.Title, app.Version}, "\x00"))
	plain, err := b.deviceSeal.Open(nil, sealed[:size], sealed[size:], aad)
	return err == nil && subtle.ConstantTimeCompare(plain, device[:]) == 1 && hmac.Equal(ticket, b.ticketHash(aad, nonce, device))
}
