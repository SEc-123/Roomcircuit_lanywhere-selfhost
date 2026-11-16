package main

import (
	"encoding/base64"
	"encoding/binary"
	"net/http"
	"net/url"
	"strconv"
	"time"
)

func generationV7(form url.Values) (uint8, bool) {
	values := form["key_generation"]
	if len(values) != 1 || len(values[0]) > 3 {
		return 0, false
	}
	num, err := strconv.ParseUint(values[0], 10, 8)
	return uint8(num), err == nil
}

func (d *deviceBootstrap) challengeV7(w http.ResponseWriter, r *http.Request) {
	now := time.Now()
	key, enrolled := d.key(r, now)
	if !enrolled {
		ndasError(w, 401, "0004", "Unauthorized device.")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 512)
	if r.ParseForm() != nil {
		ndasError(w, 400, "0014", "Invalid parameter in request.")
		return
	}
	generation, valid := generationV7(r.PostForm)
	if !valid {
		ndasError(w, 400, "0014", "Invalid parameter in request.")
		return
	}
	nonce := randomBytes(35)
	nonce[0], nonce[5] = 2, 0
	binary.BigEndian.PutUint32(nonce[1:5], uint32(now.Unix()))
	challenge := base64.RawURLEncoding.EncodeToString(nonce)
	if d.keepChallenge(w, key, challenge, now.Add(time.Minute), 7, generation) {
		respond(w, 200, map[string]string{
			"challenge": challenge, "data": base64.RawURLEncoding.EncodeToString(randomBytes(16)),
		})
	}
}

func decodeV7(text string, size int) ([]byte, bool) {
	if len(text) != base64.RawURLEncoding.EncodedLen(size) && len(text) != base64.URLEncoding.EncodedLen(size) {
		return nil, false
	}
	encoding := base64.RawURLEncoding
	if len(text)%4 == 0 {
		encoding = base64.URLEncoding
	}
	decoded, err := encoding.Strict().DecodeString(text)
	return decoded, err == nil && len(decoded) == size
}

func (d *deviceBootstrap) deviceAuthV7(w http.ResponseWriter, r *http.Request) {
	key, enrolled := d.key(r, time.Now())
	if !enrolled {
		ndasError(w, 401, "0004", "Unauthorized device.")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if r.ParseForm() != nil {
		ndasError(w, 400, "0014", "Invalid parameter in request.")
		return
	}
	for _, field := range []string{"challenge", "client_id", "ist", "system_version", "mac"} {
		if len(r.PostForm[field]) != 1 {
			ndasError(w, 400, "0014", "Invalid parameter in request.")
			return
		}
	}
	challenge, challengeOK := decodeV7(r.PostForm.Get("challenge"), 35)
	_, macShapeOK := decodeV7(r.PostForm.Get("mac"), 16)
	region, version := r.PostForm.Get("ist"), r.PostForm.Get("system_version")
	generation, generationOK := generationV7(r.PostForm)
	if !challengeOK || !macShapeOK || !generationOK || (region != "true" && region != "false") || len(version) == 0 || len(version) > 256 {
		ndasError(w, 400, "0014", "Invalid parameter in request.")
		return
	}
	clientID := r.PostForm.Get("client_id")
	if clientID != d.clientID && clientID != hos19AccountApplet && clientID != hos19AccountAPI {
		ndasError(w, 400, "0011", "Invalid client.")
		return
	}
	// The form's platform MAC is not an authority in this realm. The TLS
	// handshake proved the enrolled key and this nonce binds the request.
	d.completeDeviceChallenge(w, key, nativeDevice(key, r.TLS.PeerCertificates[0], region == "true"), clientID,
		base64.RawURLEncoding.EncodeToString(challenge), 7, generation)
}
