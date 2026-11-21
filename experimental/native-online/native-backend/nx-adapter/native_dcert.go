// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math/big"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"
)

const (
	hos19AccountApplet = "75fe236362ff5f8b"
	hos19AccountAPI    = "81333c548b2e876d"
	hos19GameDevice    = "8f849b5d34778d8e"
)

type nativeDeviceIdentity struct {
	id, serial, thumbprint string
	ist                    bool
}

type nativeDCERT struct {
	key         *rsa.PrivateKey
	kid, header string
}

var nativeDeviceSerial = regexp.MustCompile(`^[A-Z]{3}[0-9]{11}$`)

func (d *deviceBootstrap) loadDCERT() error {
	keyOrigin := os.Getenv("NX_LAB_DCERT_ORIGIN")
	if keyOrigin == "" {
		keyOrigin = d.adapter.issuer
	}
	if _, valid := origin(keyOrigin, "https"); !valid {
		return errors.New("device signing keys require an owned HTTPS origin")
	}
	privateKey, err := key(os.Getenv("NX_LAB_DAUTH_SIGNING_KEY"))
	if err != nil {
		return fmt.Errorf("load independent DAUTH signing key: %w", err)
	}
	if privateKey.N.Cmp(d.adapter.rsaKey.N) == 0 {
		return errors.New("DAUTH and BAAS require distinct signing keys")
	}
	kid := ndasKeyID("roomcircuit-device-key-v1", &privateKey.PublicKey)
	header, err := ndasHeader(keyOrigin, kid)
	if err != nil {
		return err
	}
	d.dcert = &nativeDCERT{key: privateKey, kid: kid, header: header}
	return nil
}

func ndasKeyID(purpose string, key *rsa.PublicKey) string {
	fingerprint := sha256.Sum256(append([]byte(purpose+"\x00"), x509.MarshalPKCS1PublicKey(key)...))
	return hex.EncodeToString(fingerprint[:16])
}

func ndasHeader(origin, kid string) (string, error) {
	header, err := json.Marshal(map[string]string{"alg": "RS256", "typ": "JWT", "kid": kid, "jku": origin + "/keys"})
	return jwtEncoding.EncodeToString(header), err
}

func signNDAS(key *rsa.PrivateKey, header string, claims map[string]any) (string, error) {
	body, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	unsigned := header + "." + jwtEncoding.EncodeToString(body)
	digest := sha256.Sum256([]byte(unsigned))
	signature, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, digest[:])
	if err != nil {
		return "", err
	}
	return unsigned + "." + jwtEncoding.EncodeToString(signature), nil
}

func ndasJWK(key *rsa.PublicKey, kid string) map[string]string {
	return map[string]string{
		"kty": "RSA", "alg": "RS256", "use": "sig", "kid": kid,
		"n": jwtEncoding.EncodeToString(key.N.Bytes()), "e": jwtEncoding.EncodeToString(big.NewInt(int64(key.E)).Bytes()),
	}
}

func ndasError(w http.ResponseWriter, status int, code, message string) {
	respond(w, status, map[string]any{"errors": []map[string]string{{"code": code, "message": message}}})
}

func (a *adapter) ndasPublicKeys(w http.ResponseWriter, _ *http.Request) {
	keys := make([]map[string]string, 0, 2)
	if a.bootstrap != nil && a.bootstrap.dcert != nil {
		keys = append(keys, ndasJWK(&a.bootstrap.dcert.key.PublicKey, a.bootstrap.dcert.kid))
	}
	if a.aauth != nil {
		keys = append(keys, ndasJWK(&a.rsaKey.PublicKey, a.aauth.kid))
	}
	respond(w, 200, map[string]any{"keys": keys})
}

func nativeDevice(key deviceKey, leaf *x509.Certificate, ist bool) nativeDeviceIdentity {
	binding := sha256.Sum256(append([]byte("roomcircuit-device-identity-v1\x00"), key[:]...))
	serial := fmt.Sprintf("RCN%011d", binary.BigEndian.Uint64(binding[8:16])%100000000000)
	var thumbprint string
	if leaf != nil {
		thumbprintBytes := sha256.Sum256(leaf.Raw)
		thumbprint = jwtEncoding.EncodeToString(thumbprintBytes[:])
		if nativeDeviceSerial.MatchString(leaf.Subject.SerialNumber) {
			serial = leaf.Subject.SerialNumber
		}
	}
	return nativeDeviceIdentity{id: hex.EncodeToString(binding[:8]), serial: serial, thumbprint: thumbprint, ist: ist}
}

func (i nativeDeviceIdentity) claims(deviceField string) map[string]any {
	return map[string]any{"sn": i.serial, "pc": "HAC", "dt": "NX Prod 1", "ist": i.ist,
		deviceField: i.id, "x5t#S256": i.thumbprint}
}

func (d *deviceBootstrap) signDevice(identity nativeDeviceIdentity, clientID string, now, expires time.Time) (string, error) {
	device := identity.claims("did")
	device["esv"] = jwtEncoding.EncodeToString(randomBytes(33))
	return signNDAS(d.dcert.key, d.dcert.header, map[string]any{
		"iss": d.host, "sub": identity.id, "aud": clientID, "client_id": clientID, "jti": uuid4(),
		"iat": now.Unix(), "exp": expires.Unix(), "nintendo": device,
	})
}

func (d *deviceBootstrap) expiredTitleDeviceToken(token string, now time.Time) bool {
	if d == nil || d.dcert == nil || len(token) > 2048 {
		return false
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 || parts[0] != d.dcert.header {
		return false
	}
	signature, err := jwtEncoding.DecodeString(parts[2])
	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	if err != nil || rsa.VerifyPKCS1v15(&d.dcert.key.PublicKey, crypto.SHA256, digest[:], signature) != nil {
		return false
	}
	payload, err := jwtEncoding.DecodeString(parts[1])
	var claim struct {
		Issuer   string `json:"iss"`
		Audience string `json:"aud"`
		ClientID string `json:"client_id"`
		Issued   int64  `json:"iat"`
		Expires  int64  `json:"exp"`
	}
	return err == nil && json.Unmarshal(payload, &claim) == nil &&
		claim.Issuer == d.host && claim.Audience == d.clientID && claim.ClientID == d.clientID &&
		claim.Issued > 0 && claim.Expires > claim.Issued && now.Unix() >= claim.Expires
}

func (d *deviceBootstrap) signAnonymous(issued deviceProof, now, expires time.Time) (string, error) {
	identity := issued.identity
	if identity.id == "" {
		identity = nativeDevice(issued.key, nil, false)
	}
	header, audience := jwtHeader, d.clientID
	if d.adapter.baasKeys != nil {
		header, audience = d.adapter.baasKeys.accessHeader, d.adapter.baasKeys.audience
	}
	return signNDAS(d.adapter.rsaKey, header, map[string]any{
		"iss": d.adapter.issuer, "aud": audience, "sub": identity.id, "typ": "token", "jti": uuid4(),
		"iat": now.Unix(), "exp": expires.Unix(), "bs:did": randomHex(8), "bs:grt": 1,
		"bs:pid": "pz" + randomHex(15), "bs:sts": []int64{1188950301625811329},
		"nintendo": identity.claims("di"),
	})
}
