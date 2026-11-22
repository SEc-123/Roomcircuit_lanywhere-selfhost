package main

import (
	"context"
	"crypto"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math/big"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const naRefreshTTL = 30 * 24 * time.Hour

type naProof struct {
	Issuer       string   `json:"iss"`
	Audience     string   `json:"aud"`
	Subject      string   `json:"sub"`
	Issued       int64    `json:"iat"`
	Expires      int64    `json:"exp"`
	JWTID        string   `json:"jti"`
	Purpose      string   `json:"typ,omitempty"`
	Nonce        string   `json:"nonce,omitempty"`
	AccessHash   string   `json:"at_hash,omitempty"`
	Grant        string   `json:"rc:grant"`
	Scopes       []string `json:"rc:scopes"`
	Device       string   `json:"rc:device,omitempty"`
	Sealed       string   `json:"rc:sealed,omitempty"`
	GrantType    int      `json:"ac:grt,omitempty"`
	NativeScopes []int    `json:"ac:scp,omitempty"`
}

type naIssuer struct {
	origin, kid, header string
	audiences           map[string]*url.URL
	key                 *rsa.PrivateKey
	seal                cipher.AEAD
}

func newNAIssuer(origin, sealingKey string, audiences map[string]*url.URL, key *rsa.PrivateKey) (*naIssuer, error) {
	fingerprint := sha256.Sum256(x509.MarshalPKCS1PublicKey(&key.PublicKey))
	kid := hex.EncodeToString(fingerprint[:16])
	header, err := json.Marshal(map[string]string{"alg": "RS256", "typ": "JWT", "jku": origin + "/1.0.0/certificates", "kid": kid})
	if err != nil {
		return nil, err
	}
	derived := sha256.Sum256([]byte("roomcircuit-na-refresh-v1\x00" + sealingKey))
	block, err := aes.NewCipher(derived[:])
	if err != nil {
		return nil, err
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	return &naIssuer{origin, kid, jwtEncoding.EncodeToString(header), audiences, key, aead}, nil
}

func (n *naIssuer) certificates(w http.ResponseWriter, _ *http.Request) {
	pub := n.key.PublicKey
	respond(w, 200, map[string]any{"keys": []map[string]string{{"kty": "RSA", "alg": "RS256", "use": "sig", "kid": n.kid,
		"n": jwtEncoding.EncodeToString(pub.N.Bytes()), "e": jwtEncoding.EncodeToString(big.NewInt(int64(pub.E)).Bytes())}}})
}

func naSubject(who player) string {
	// The native network-account namespace is stable across devices and is
	// separate from the BAAS/game PID namespace.
	digest := sha256.Sum256([]byte("roomcircuit-network-account-v1\x00" + who.Account))
	return hex.EncodeToString(digest[:8])
}

func uuid4() string {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		panic(err)
	}
	raw[6] = raw[6]&0x0f | 0x40
	raw[8] = raw[8]&0x3f | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", raw[:4], raw[4:6], raw[6:8], raw[8:10], raw[10:])
}

func (n *naIssuer) signed(p naProof) (string, error) {
	body, err := json.Marshal(p)
	if err != nil {
		return "", err
	}
	unsigned := n.header + "." + jwtEncoding.EncodeToString(body)
	digest := sha256.Sum256([]byte(unsigned))
	sig, err := rsa.SignPKCS1v15(rand.Reader, n.key, crypto.SHA256, digest[:])
	if err != nil {
		return "", err
	}
	return unsigned + "." + jwtEncoding.EncodeToString(sig), nil
}

func (n *naIssuer) verify(encoded, purpose string, allowExpired bool) (naProof, bool) {
	var p naProof
	if len(encoded) > 3072 {
		return p, false
	}
	parts := strings.Split(encoded, ".")
	if len(parts) != 3 || parts[0] != n.header {
		return p, false
	}
	sig, err := jwtEncoding.DecodeString(parts[2])
	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	if err != nil || rsa.VerifyPKCS1v15(&n.key.PublicKey, crypto.SHA256, digest[:], sig) != nil {
		return p, false
	}
	body, err := jwtEncoding.DecodeString(parts[1])
	if err != nil || json.Unmarshal(body, &p) != nil {
		return p, false
	}
	now := time.Now().Unix()
	subject, subErr := hex.DecodeString(p.Subject)
	grant, grantErr := hex.DecodeString(p.Grant)
	return p, p.Issuer == n.origin && n.audiences[p.Audience] != nil && p.Purpose == purpose && p.Expires > p.Issued &&
		p.Issued <= now && (allowExpired || now < p.Expires) && p.JWTID != "" && subErr == nil && len(subject) == 8 && grantErr == nil && len(grant) == 24
}

func (n *naIssuer) wrap(p naProof, secret string) (string, error) {
	nonce := make([]byte, n.seal.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return "", err
	}
	p.Sealed = jwtEncoding.EncodeToString(n.seal.Seal(nonce, nonce, []byte(secret), []byte(p.JWTID)))
	return n.signed(p)
}

func (n *naIssuer) unwrap(p naProof) (string, bool) {
	blob, err := jwtEncoding.DecodeString(p.Sealed)
	size := n.seal.NonceSize()
	if err != nil || len(blob) != size+64+n.seal.Overhead() {
		return "", false
	}
	data, err := n.seal.Open(nil, blob[:size], blob[size:], []byte(p.JWTID))
	return string(data), err == nil
}

func naDeviceIdentity(device deviceKey) string {
	if device == (deviceKey{}) {
		return ""
	}
	value := sha256.Sum256(append([]byte("roomcircuit-na-enrollment-v1\x00"), device[:]...))
	return hex.EncodeToString(value[:])
}

func (a *adapter) federationGrant(ctx context.Context, token string, enrollment deviceKey) (player, int) {
	if a.na != nil {
		if proof, ok := a.na.verify(token, "", false); ok {
			if proof.Device != "" && proof.Device != naDeviceIdentity(enrollment) {
				return player{}, 403
			}
			return a.naPlayer(ctx, proof)
		}
	}
	if a.bootstrap != nil {
		return player{}, 401
	}
	return a.checkGrant(ctx, token)
}

func (a *adapter) naPlayer(ctx context.Context, p naProof) (player, int) {
	who, status := a.revalidateGrant(ctx, durableGrantPrefix+p.Grant)
	if status == 200 && p.Subject != naSubject(who) {
		return player{}, 403
	}
	return who, status
}

func (a *adapter) authenticateNAAccess(ctx context.Context, token string) (player, int) {
	if a.na == nil {
		return player{}, 401
	}
	p, ok := a.na.verify(token, "token", false)
	if !ok {
		return player{}, 401
	}
	return a.naPlayer(ctx, p)
}
