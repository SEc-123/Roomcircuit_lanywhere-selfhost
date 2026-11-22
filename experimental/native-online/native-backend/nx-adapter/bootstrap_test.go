package main

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"image"
	"image/jpeg"
	"io"
	"math/big"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func wireCertificate(t *testing.T, usage x509.ExtKeyUsage, hostnames ...string) (tls.Certificate, *x509.Certificate) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "isolated TLS fixture"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{usage}, DNSNames: hostnames,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key, Leaf: leaf}, leaf
}

func TestNativeBootstrapWire(t *testing.T) {
	const authHost, accountHost, dcertHost = "device.roomcircuit.test", "accounts.roomcircuit.test", "dcert.roomcircuit.test"
	const baasHost = "baas-keys.roomcircuit.test"
	const clientID, app = hos19GameDevice, "0000000000001234"
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	validA, certA := wireCertificate(t, x509.ExtKeyUsageClientAuth)
	validB, certB := wireCertificate(t, x509.ExtKeyUsageClientAuth)
	unknown, _ := wireCertificate(t, x509.ExtKeyUsageClientAuth)
	serverCert, serverLeaf := wireCertificate(t, x509.ExtKeyUsageServerAuth, authHost, accountHost, dcertHost, baasHost)
	pinA, pinB := sha256.Sum256(certA.RawSubjectPublicKeyInfo), sha256.Sum256(certB.RawSubjectPublicKeyInfo)
	roots := x509.NewCertPool()
	roots.AddCert(serverLeaf)
	issuer := "https://" + accountHost
	ownGrant, otherGrant := strings.Repeat("a", 48), strings.Repeat("b", 48)
	own := player{PID: 1, Account: "own-account", User: "0000000000001238", Nickname: "Own", Country: "US", Application: app, GrantID: ownGrant}
	other := player{PID: 2, Account: "other-account", User: "0000000000001239", Nickname: "Other", Country: "GB", Application: app, GrantID: otherGrant}
	grantServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var request struct {
			ID string `json:"grant_id"`
		}
		if json.NewDecoder(r.Body).Decode(&request) != nil {
			w.WriteHeader(400)
			return
		}
		switch request.ID {
		case ownGrant:
			respond(w, 200, own)
		case otherGrant:
			respond(w, 200, other)
		default:
			w.WriteHeader(401)
		}
	}))
	defer grantServer.Close()
	dir := t.TempDir()
	statePath, stateKey := filepath.Join(dir, "device-state"), filepath.Join(dir, "device-key")
	if err := os.WriteFile(stateKey, []byte(randomHex(32)), 0o600); err != nil {
		t.Fatal(err)
	}
	a := &adapter{
		issuer: issuer, application: app, rsaKey: key, grantURL: grantServer.URL,
		client: grantServer.Client(), sessions: make(map[string]session), admitted: make(map[uint64]gameAdmission), gameKey: randomHex(32),
	}
	naKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	const titleClient = "1122334455667700"
	a.na, err = newNAIssuer(issuer, "fixture-wrapping-key", map[string]*url.URL{
		clientID: {Scheme: "roomcircuit", Host: "auth"}, titleClient: {Scheme: "npf" + titleClient, Host: "auth"}}, naKey)
	if err != nil {
		t.Fatal(err)
	}
	idToken := func(who player) string {
		now := time.Now()
		token, err := a.na.signed(naProof{Issuer: issuer, Audience: clientID, Subject: naSubject(who), Issued: now.Unix(), Expires: now.Add(time.Minute).Unix(), JWTID: uuid4(), Grant: who.GrantID})
		if err != nil {
			t.Fatal(err)
		}
		return token
	}
	a.store, a.devices, err = openDeviceStore(statePath, stateKey, app)
	if err != nil {
		t.Fatal(err)
	}
	defer a.store.lock.Close()
	t.Setenv("NX_LAB_DEVICE_HOST", authHost)
	t.Setenv("NX_LAB_DEVICE_CLIENT_ID", "")
	t.Setenv("NX_LAB_ENROLLED_SPKI", hex.EncodeToString(pinA[:])+","+hex.EncodeToString(pinB[:]))
	t.Setenv("NX_LAB_TITLE_OAUTH_CLIENT_ID", titleClient)
	t.Setenv("NX_LAB_BAAS_KEY_ORIGIN", "https://"+baasHost)
	t.Setenv("NX_LAB_DCERT_ORIGIN", "https://"+dcertHost)
	dauthKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(dauthKey)
	if err != nil {
		t.Fatal(err)
	}
	dauthKeyPath := filepath.Join(dir, "dauth-issuer-key.pem")
	if err := os.WriteFile(dauthKeyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("NX_LAB_DAUTH_SIGNING_KEY", dauthKeyPath)
	mux := http.NewServeMux()
	if err := a.addNativeBootstrap(mux); err != nil {
		t.Fatal(err)
	}
	if err := a.addNativeBAASRoutes(mux); err != nil {
		t.Fatal(err)
	}
	if err := a.addBAASCertificates(mux); err != nil {
		t.Fatal(err)
	}
	mux.HandleFunc("POST /1.0.0/users", a.registerDevice)
	mux.HandleFunc("POST /1.0.0/federation", a.deviceSession)
	mux.HandleFunc("POST /1.0.0/login", a.deviceSession)
	srv := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if (strings.HasPrefix(r.URL.Path, "/v7/") || strings.HasPrefix(r.URL.Path, "/v5/")) &&
			(r.UserAgent() != "" || r.Header.Get("X-Nintendo-PowerState") != "FA") {
			w.WriteHeader(418)
			return
		}
		mux.ServeHTTP(w, r)
	}))
	srv.TLS = a.bootstrap.tlsConfig(serverCert)
	srv.StartTLS()
	defer srv.Close()

	newClient := func(host string, cert *tls.Certificate, requested *atomic.Int32) *http.Client {
		conf := &tls.Config{RootCAs: roots, ServerName: host, MinVersion: tls.VersionTLS12}
		if cert != nil {
			conf.GetClientCertificate = func(*tls.CertificateRequestInfo) (*tls.Certificate, error) {
				requested.Add(1)
				return cert, nil
			}
		}
		transport := &http.Transport{TLSClientConfig: conf}
		t.Cleanup(transport.CloseIdleConnections)
		return &http.Client{Transport: transport, Timeout: 5 * time.Second}
	}
	var accountCerts, authCerts atomic.Int32
	account := newClient(accountHost, &validA, &accountCerts)
	baasKeys := newClient(baasHost, nil, nil)
	dcert := newClient(dcertHost, nil, nil)
	deviceA, deviceB := newClient(authHost, &validA, &authCerts), newClient(authHost, &validB, &authCerts)
	noCert, wrong := newClient(authHost, nil, nil), newClient(authHost, &unknown, &authCerts)
	call := func(client *http.Client, method, path, mime, body, token string, status int) (string, http.Header) {
		t.Helper()
		req, err := http.NewRequest(method, srv.URL+path, strings.NewReader(body))
		if err != nil {
			t.Fatal(err)
		}
		req.Host = client.Transport.(*http.Transport).TLSClientConfig.ServerName
		req.Header.Set("Content-Type", mime)
		if strings.HasPrefix(path, "/v7/") || strings.HasPrefix(path, "/v5/") {
			req.Header.Set("User-Agent", "")
			req.Header.Set("X-Nintendo-PowerState", "FA")
			req.Header.Set("Accept", "*/*")
		}
		if token != "" {
			req.Header.Set("Authorization", "Bearer "+token)
		}
		req.Header.Set("X-Client-Certificate", hex.EncodeToString(pinA[:]))
		res, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer res.Body.Close()
		data, err := io.ReadAll(res.Body)
		if err != nil {
			t.Fatal(err)
		}
		if res.StatusCode != status {
			t.Fatalf("%s expected %d got %d", path, status, res.StatusCode)
		}
		return string(data), res.Header
	}
	const challengePath, tokenPath = "/v8/challenge", "/v8/device_auth_tokens"
	const applicationPath, form = "/1.0.0/application/token", "application/x-www-form-urlencoded"
	post := http.MethodPost
	call(noCert, post, challengePath, "", "", "", 401)
	call(wrong, post, challengePath, "", "", "", 401)
	call(account, post, challengePath, "", "", "", 401)
	res, _ := call(account, "GET", "/2.0.0/agreements", "", "", "", 200)
	if strings.TrimSpace(res) != `{"agreements":[]}` || accountCerts.Load() != 0 {
		t.Fatal("the account host requested a client certificate or served the wrong agreements form")
	}
	call(account, post, "/2.0.0/agreements", form, "", "", 204)
	exchange := func(deviceAuth string) string {
		t.Helper()
		arguments := url.Values{"assertion": {deviceAuth}, "grantType": {"public_client"}}.Encode()
		call(account, post, applicationPath, form, "grantType=public_client&assertion=invalid", "", 401)
		call(account, post, applicationPath, form, "assertion=missing-grant-type", "", 400)
		call(account, post, applicationPath, form, arguments+"&assertion=invalid", "", 400)
		response, _ := call(account, post, applicationPath, form, arguments, "", 200)
		retry, _ := call(account, post, applicationPath, form, arguments, "", 200)
		var application, repeated struct {
			AccessToken string
			TokenType   string
			ExpiresIn   int
		}
		if json.Unmarshal([]byte(response), &application) != nil || strings.Count(application.AccessToken, ".") != 2 ||
			application.TokenType != "Bearer" || application.ExpiresIn < 1 {
			t.Fatal("invalid application token response")
		}
		if json.Unmarshal([]byte(retry), &repeated) != nil || repeated.AccessToken != application.AccessToken ||
			repeated.TokenType != "Bearer" || repeated.ExpiresIn < 1 || repeated.ExpiresIn > application.ExpiresIn {
			t.Fatal("retry did not return the original application token with remaining lifetime")
		}
		return application.AccessToken
	}
	bootstrapV8 := func(client, other *http.Client) string {
		t.Helper()
		body, _ := call(client, post, challengePath, "", "", "", 200)
		var challenge struct{ Challenge, Data string }
		if json.Unmarshal([]byte(body), &challenge) != nil || len(challenge.Challenge) != 44 || len(challenge.Data) != 24 {
			t.Fatal("invalid v8 challenge shape")
		}
		body = `{"system_version":"00140001","fw_revision":"fixture","key_generation":20,"challenge":"` + challenge.Challenge +
			`","mac":"wire fixture","ist":false,"token_requests":[{"client_id":"` + clientID + `"}]}`
		call(client, post, tokenPath, "application/json", body+"{}", "", 400)
		call(other, post, tokenPath, "application/json", body, "", 401)
		call(client, post, tokenPath, "application/json", strings.ReplaceAll(body, clientID, "unknown"), "", 400)
		res, _ := call(client, post, tokenPath, "application/json", body, "", 200)
		call(client, post, tokenPath, "application/json", body, "", 401)
		var result struct {
			Results []struct {
				ClientID string `json:"client_id"`
				Token    string `json:"device_auth_token"`
				Expires  int    `json:"expires_in"`
			} `json:"results"`
		}
		if json.Unmarshal([]byte(res), &result) != nil || len(result.Results) != 1 || result.Results[0].ClientID != clientID ||
			strings.Count(result.Results[0].Token, ".") != 2 || result.Results[0].Expires < 1 {
			t.Fatal("invalid v8 device auth token response")
		}
		return result.Results[0].Token
	}
	const challengeV7, tokenV7 = "/v7/challenge", "/v7/device_auth_token"
	call(noCert, post, challengeV7, form, "key_generation=19", "", 401)
	call(wrong, post, challengeV7, form, "key_generation=19", "", 401)
	call(deviceA, post, challengeV7, form, "key_generation=19&key_generation=1", "", 400)
	packet, _ := call(deviceA, post, challengeV7, form, "key_generation=19", "", 200)
	var challenge struct{ Challenge, Data string }
	if json.Unmarshal([]byte(packet), &challenge) != nil {
		t.Fatal("invalid v7 challenge envelope")
	}
	nonce, nonceErr := jwtEncoding.DecodeString(challenge.Challenge)
	data, dataErr := jwtEncoding.DecodeString(challenge.Data)
	if nonceErr != nil || dataErr != nil || len(nonce) != 35 || len(data) != 16 || nonce[0] != 2 {
		t.Fatal("invalid HOS19 v7 challenge shape")
	}
	args := url.Values{"challenge": {challenge.Challenge}, "client_id": {clientID}, "ist": {"false"},
		"key_generation": {"19"}, "system_version": {"fixture#00130001#example"}, "mac": {jwtEncoding.EncodeToString(make([]byte, 16))}}
	call(deviceB, post, tokenV7, form, args.Encode(), "", 401)
	call(deviceA, post, tokenV7, form, args.Encode()+"&client_id=duplicate", "", 400)
	args.Set("key_generation", "18")
	call(deviceA, post, tokenV7, form, args.Encode(), "", 401)
	args.Set("key_generation", "19")
	args.Set("client_id", "0000000000000000")
	badClient, _ := call(deviceA, post, tokenV7, form, args.Encode(), "", 400)
	verifyNDASError(t, badClient, "0011")
	args.Set("client_id", clientID)
	packet, _ = call(deviceA, post, tokenV7, form, args.Encode(), "", 200)
	replayed, _ := call(deviceA, post, tokenV7, form, args.Encode(), "", 401)
	verifyNDASError(t, replayed, "0004")
	var legacy struct {
		Assertion string `json:"device_auth_token"`
		Expires   int    `json:"expires_in"`
	}
	if json.Unmarshal([]byte(packet), &legacy) != nil || strings.Count(legacy.Assertion, ".") != 2 || legacy.Expires < 1 {
		t.Fatal("invalid v7 singular token response")
	}
	proofA, proofB := legacy.Assertion, bootstrapV8(deviceB, deviceA)
	keySet, _ := call(dcert, "GET", "/keys", "", "", "", 200)
	jku := "https://" + dcertHost + "/keys"
	claims := claimsFromOwnKeySet(t, proofA, keySet, jku)
	deviceClaims, ok := claims["nintendo"].(map[string]any)
	expectedIdentity := nativeDevice(deviceKey(pinA), certA, false)
	if claims["iss"] != authHost || claims["aud"] != clientID || claims["client_id"] != clientID ||
		claims["sub"] != expectedIdentity.id || !ok || deviceClaims["did"] != expectedIdentity.id ||
		deviceClaims["sn"] != expectedIdentity.serial || deviceClaims["x5t#S256"] != expectedIdentity.thumbprint {
		t.Fatal("DAUTH proof does not bind the TLS device identity and audience")
	}
	for _, client := range []string{hos19AccountApplet, hos19AccountAPI} {
		msg, _ := call(deviceA, post, challengeV7, form, "key_generation=19", "", 200)
		if json.Unmarshal([]byte(msg), &challenge) != nil {
			t.Fatal("account client did not receive a v7 challenge")
		}
		args.Set("challenge", challenge.Challenge)
		args.Set("client_id", client)
		msg, _ = call(deviceA, post, tokenV7, form, args.Encode(), "", 200)
		if json.Unmarshal([]byte(msg), &legacy) != nil {
			t.Fatal("account role did not receive a v7 token")
		}
		claims := claimsFromOwnKeySet(t, legacy.Assertion, keySet, jku)
		if claims["aud"] != client || claims["sub"] != expectedIdentity.id {
			t.Fatal("DAUTH account client lost its separate audience")
		}
		if enrolled, ok := a.bootstrap.accountDeviceAuthorization(legacy.Assertion); !ok || enrolled != deviceKey(pinA) {
			t.Fatal("account-specific device assertion was not recognized")
		}
		raw := url.Values{"assertion": {legacy.Assertion}, "grantType": {"public_client"}}.Encode()
		call(account, post, applicationPath, form, raw, "", 403)
		title := url.Values{"device_auth_token": {legacy.Assertion}, "application_id": {app}, "application_version": {"00000033"}}
		denied, _ := call(account, post, "/v5/application_auth_token", form, title.Encode(), "", 401)
		verifyNDASError(t, denied, "0106")
	}
	if _, ok := a.bootstrap.accountDeviceAuthorization(proofA); ok {
		t.Fatal("game device proof crossed into the account API role")
	}
	tokenA, tokenB := exchange(proofA), exchange(proofB)
	baasSet, _ := call(baasKeys, "GET", "/1.0.0/internal_certificates", "", "", "", 200)
	anonymous := claimsFromOwnKeySet(t, tokenA, baasSet, "https://"+baasHost+"/1.0.0/internal_certificates")
	if anonymous["typ"] != "token" || anonymous["bs:grt"] != float64(1) || anonymous["sub"] != expectedIdentity.id {
		t.Fatal("BAAS anonymous assertion lost its device-only role")
	}
	if authCerts.Load() < 2 {
		t.Fatal("device TLS listener did not request certificate proof")
	}
	call(account, post, "/1.0.0/users", "", "", "", 401)
	res, header := call(account, post, "/1.0.0/users", "", "", tokenA, 201)
	var registered struct {
		ID             string
		DeviceAccounts []struct{ ID, Password string }
	}
	lower16 := regexp.MustCompile(`^[0-9a-f]{16}$`)
	if json.Unmarshal([]byte(res), &registered) != nil || !lower16.MatchString(registered.ID) ||
		len(registered.DeviceAccounts) != 1 || !lower16.MatchString(registered.DeviceAccounts[0].ID) ||
		len(registered.DeviceAccounts[0].Password) < 20 || header.Get("Location") != "/1.0.0/users/"+registered.ID {
		t.Fatal("invalid unlinked user registration shape")
	}
	gameAdmission := func(body string, expected int) {
		t.Helper()
		var logged struct {
			IDToken string `json:"idToken"`
		}
		if json.Unmarshal([]byte(body), &logged) != nil || logged.IDToken == "" {
			t.Fatal("missing BAAS ID token")
		}
		parts := strings.Split(logged.IDToken, ".")
		if len(parts) != 3 {
			t.Fatal("BAAS token envelope missing")
		}
		decoded, err := jwtEncoding.DecodeString(parts[1])
		var issued proof
		if err != nil || json.Unmarshal(decoded, &issued) != nil || (issued.Application != nil) != (expected == 200) {
			t.Fatal("title metadata did not match actual device authorization")
		}
		if expected == 200 && (issued.Application.Title != app || issued.Application.Version != "00000033" ||
			!a.baasKeys.validTitleTicket(issued, deviceKey(pinA), app) || a.baasKeys.validTitleTicket(issued, deviceKey(pinB), app)) {
			t.Fatal("BAAS did not bind the game title and ticket to the correct enrolled device")
		}
		encoded, _ := json.Marshal(map[string]string{"id_token": logged.IDToken, "application_id": app})
		req := httptest.NewRequest(post, "/internal/switch/identity", strings.NewReader(string(encoded)))
		req.Header.Set("X-Internal-Key", a.gameKey)
		rec := httptest.NewRecorder()
		a.gameIdentity(rec, req)
		if rec.Code != expected {
			t.Fatalf("private game identity returned %d, expected %d", rec.Code, expected)
		}
	}
	profile := registered.DeviceAccounts[0]
	formCreds := url.Values{"id": {profile.ID}, "password": {profile.Password}}
	call(account, post, "/1.0.0/login", form, formCreds.Encode(), tokenB, 401)
	prelink, _ := call(account, post, "/1.0.0/login", form, formCreds.Encode(), tokenA, 200)
	var localLogin struct {
		AccessToken string
		User        struct{ ID string }
	}
	if json.Unmarshal([]byte(prelink), &localLogin) != nil || localLogin.AccessToken == "" || localLogin.User.ID != registered.ID {
		t.Fatal("own provisional login lost the registered device user")
	}
	gameAdmission(prelink, 404)
	profileURL := "/1.0.0/users/" + registered.ID
	call(account, "GET", profileURL, "", "", localLogin.AccessToken, 200)
	call(account, "GET", "/1.0.0/users/0000000000000011", "", "", localLogin.AccessToken, 403)
	patch := `[{"op":"replace","path":"/nickname","value":"Native Profile"},{"op":"replace","path":"/country","value":"GB"},{"op":"replace","path":"/birthday","value":"1992-05-10"}]`
	prelinkProfile, _ := call(account, "PATCH", profileURL, "application/json", patch, localLogin.AccessToken, 200)
	if !strings.Contains(prelinkProfile, "Native Profile") || !strings.Contains(prelinkProfile, "1992-05-10") {
		t.Fatal("provisional profile edit was not stored under the registered ID")
	}
	var picture bytes.Buffer
	if err := jpeg.Encode(&picture, image.NewGray(image.Rect(0, 0, 16, 16)), nil); err != nil {
		t.Fatal(err)
	}
	upload, _ := json.Marshal(map[string]any{"rawContent": base64.StdEncoding.EncodeToString(picture.Bytes()), "ownerId": registered.ID, "allowTransform": true})
	call(account, post, "/1.0.0/image_upload", "application/json", string(upload), localLogin.AccessToken, 200)
	call(account, "GET", "/1/"+registered.ID, "", "", localLogin.AccessToken, 200)
	call(account, "GET", "/v1/users/"+registered.ID+"/membership?application_id="+app, "", "", tokenA, 401)
	call(account, "GET", "/v1/users/"+registered.ID+"/membership?application_id="+app, "", "", localLogin.AccessToken, 404)
	formCreds.Set("idToken", "private-broker-token")
	rejection, _ := call(account, post, "/1.0.0/federation", form, formCreds.Encode(), tokenA, 400)
	var invalidIDP map[string]any
	if json.Unmarshal([]byte(rejection), &invalidIDP) != nil || len(invalidIDP) != 6 || invalidIDP["errorCode"] != "invalid_idp" ||
		invalidIDP["instance"] != "/1.0.0/federation" || strings.Contains(rejection, formCreds.Get("idToken")) {
		t.Fatal("missing or unsafe native provider error envelope")
	}
	formCreds.Set("idp", "differentAccount")
	call(account, post, "/1.0.0/federation", form, formCreds.Encode(), tokenA, 400)
	formCreds.Set("idp", "nintendoAccount")
	call(account, post, "/1.0.0/federation", form, formCreds.Encode(), tokenA, 400)
	formCreds.Set("idToken", idToken(own))
	call(account, post, "/1.0.0/federation", form, formCreds.Encode(), tokenB, 401)
	call(account, post, "/1.0.0/federation", form, formCreds.Encode(), "", 401)
	pending, err := a.store.load()
	if err != nil {
		t.Fatal(err)
	}
	if entry := pending[profile.ID]; entry.provisional != registered.ID || entry.owner.PID != 0 || entry.enrollment != deviceKey(pinA) {
		t.Fatal("unlinked device not preserved as a provisional account")
	}
	res, _ = call(account, post, "/1.0.0/federation", form, formCreds.Encode(), tokenA, 200)
	var linkedLogin struct {
		AccessToken string
		User        struct {
			ID       string
			Nickname string
		}
	}
	if json.Unmarshal([]byte(res), &linkedLogin) != nil || linkedLogin.User.ID != registered.ID ||
		linkedLogin.User.Nickname != "Native Profile" || strings.Contains(res, profile.Password) {
		t.Fatal("link disclosed credentials or lost the approved own identity")
	}
	call(account, "GET", profileURL, "", "", localLogin.AccessToken, 404)
	call(account, "GET", "/1/"+registered.ID, "", "", localLogin.AccessToken, 404)
	call(account, "GET", "/1/"+registered.ID, "", "", linkedLogin.AccessToken, 200)
	postlinkProfile, _ := call(account, "GET", profileURL, "", "", linkedLogin.AccessToken, 200)
	if !strings.Contains(postlinkProfile, "Native Profile") || !strings.Contains(postlinkProfile, `"country":"US"`) {
		t.Fatal("linked profile did not keep owner edits and verified account country")
	}
	formCreds.Set("idToken", idToken(other))
	call(account, post, "/1.0.0/federation", form, formCreds.Encode(), tokenA, 403)
	formCreds.Del("idToken")
	res, _ = call(account, post, "/1.0.0/login", form, formCreds.Encode(), tokenA, 200)

	gameAdmission(res, 403)
	titleToken := func(proof string) string {
		t.Helper()
		args := url.Values{"device_auth_token": {proof}, "application_id": {app}, "application_version": {"00000033"}}
		body, _ := call(account, post, "/v5/application_auth_token", form, args.Encode(), "", 200)
		var application struct {
			Token string `json:"application_auth_token"`
		}
		if json.Unmarshal([]byte(body), &application) != nil || application.Token == "" {
			t.Fatal("missing title authorization token")
		}
		return application.Token
	}
	formCreds.Set("appAuthNToken", titleToken(proofB))
	rejection, _ = call(account, post, "/1.0.0/login", form, formCreds.Encode(), tokenA, 400)
	if !strings.Contains(rejection, "invalid_ndas_app_authn_token") {
		t.Fatal("wrong signed title proof did not return the native error code")
	}
	formCreds.Set("appAuthNToken", titleToken(proofA))
	res, _ = call(account, post, "/1.0.0/login", form, formCreds.Encode(), tokenA, 200)
	gameAdmission(res, 200)
	linked, err := a.store.load()
	if err != nil {
		t.Fatal(err)
	}
	if entry := linked[profile.ID]; entry.provisional != "" || entry.owner.PID != 1 || entry.owner.User != registered.ID || entry.enrollment != deviceKey(pinA) {
		t.Fatal("verified association was not committed for the enrolled device")
	}
	a.bootstrap.mu.Lock()
	assertionHash := sha256.Sum256([]byte(proofA))
	expired := a.bootstrap.assertions[assertionHash]
	expired.expires = time.Now().Add(-time.Second)
	a.bootstrap.assertions[assertionHash] = expired
	a.bootstrap.mu.Unlock()
	call(account, post, applicationPath, form, url.Values{"assertion": {proofA}, "grantType": {"public_client"}}.Encode(), "", 401)
	application := url.Values{"device_auth_token": {"fabricated"}, "application_id": {app}, "application_version": {"00000033"}}
	terminal, _ := call(account, post, "/v5/application_auth_token", form, application.Encode(), "", 401)
	verifyNDASError(t, terminal, "0118")
	now := time.Now()
	stale, err := a.bootstrap.signDevice(expectedIdentity, clientID, now.Add(-2*time.Minute), now.Add(-time.Minute))
	if err != nil {
		t.Fatal(err)
	}
	application.Set("device_auth_token", stale)
	expiry, _ := call(account, post, "/v5/application_auth_token", form, application.Encode(), "", 401)
	verifyNDASError(t, expiry, "0103")
}

func verifyNDASError(t *testing.T, body, expected string) {
	t.Helper()
	var response struct {
		Errors []struct{ Code, Message string }
	}
	if json.Unmarshal([]byte(body), &response) != nil || len(response.Errors) != 1 ||
		response.Errors[0].Code != expected || response.Errors[0].Message == "" {
		t.Fatalf("expected native error code %s", expected)
	}
}
