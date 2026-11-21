package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestUsernameEdgeIgnoresForgedForwardedForAndSeparatePeers(t *testing.T) {
	var l edgeLimiter
	for i := 0; i < 21; i++ {
		r := httptest.NewRequest("POST", "https://own.test/register", nil)
		r.RemoteAddr = "198.51.100.10:1234"
		r.Header.Set("X-Forwarded-For", "203.0.113."+string(rune('a'+i)))
		got := l.allow(r, "register", 20)
		if got != (i < 20) {
			t.Fatalf("forged forwarded address bypassed source limit at %d", i)
		}
	}
	r := httptest.NewRequest("POST", "https://own.test/register", nil)
	r.RemoteAddr = "198.51.100.11:1234"
	if !l.allow(r, "register", 20) {
		t.Fatal("unrelated client affected")
	}
}
func TestInternalBypassRequiresLoopbackAndSecret(t *testing.T) {
	s := &service{gameKey: strings.Repeat("k", 64)}
	for _, row := range []struct {
		address, key string
		want         bool
	}{{"127.0.0.1:1234", s.gameKey, true}, {"198.51.100.1:1234", s.gameKey, false}, {"127.0.0.1:1234", "wrong", false}} {
		r := httptest.NewRequest("POST", "https://own.test/device", nil)
		r.RemoteAddr = row.address
		r.Header.Set("X-Internal-Key", row.key)
		if s.trustedInternal(r) != row.want {
			t.Fatalf("incorrect bypass for %s", row.address)
		}
	}
}

func TestEdgeLimitsTwoRealTCPSourceAddresses(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux supports distinct 127/8 source aliases without system network changes")
	}
	var l edgeLimiter
	// Loopback aliases model two peers in an isolated host test only. Production
	// never enables this internal test flag and rejects loopback external peers.
	l.allowLoopbackForTest = true
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !l.allow(r, "register", 3) {
			w.WriteHeader(429)
			return
		}
		w.WriteHeader(200)
	}))
	server.StartTLS()
	defer server.Close()
	makeClient := func(address string) *http.Client {
		transport := server.Client().Transport.(*http.Transport).Clone()
		dialer := &net.Dialer{LocalAddr: &net.TCPAddr{IP: net.ParseIP(address)}, Timeout: 3 * time.Second}
		transport.DialContext = dialer.DialContext
		transport.DisableKeepAlives = true
		return &http.Client{Transport: transport, Timeout: 5 * time.Second}
	}
	first, second := makeClient("127.0.0.2"), makeClient("127.0.0.3")
	for i := 0; i < 4; i++ {
		req, _ := http.NewRequest("POST", server.URL, nil)
		req.Header.Set("X-Forwarded-For", fmt.Sprintf("198.51.100.%d", i+1))
		res, err := first.Do(req)
		if err != nil {
			t.Fatalf("first TCP client: %v", err)
		}
		res.Body.Close()
		want := 200
		if i == 3 {
			want = 429
		}
		if res.StatusCode != want {
			t.Fatalf("first client request %d status %d", i, res.StatusCode)
		}
	}
	req, _ := http.NewRequest("POST", server.URL, nil)
	req.Header.Set("X-Forwarded-For", "127.0.0.2")
	res, err := second.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		t.Fatalf("second source unfairly rate limited: %d", res.StatusCode)
	}
}

func TestUsernameApprovalAlwaysReleasesTemporaryLogin(t *testing.T) {
	const value = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	for _, tc := range []struct {
		name     string
		status   int
		body     string
		decision string
		want     int
	}{
		{"malformed response", 200, "{", "approve", 503},
		{"missing account", 200, `{}`, "approve", 503},
		{"error response with session", 401, `{}`, "approve", 401},
		{"player denies", 200, `{"account_id":"fixture"}`, "deny", 200},
	} {
		t.Run(tc.name, func(t *testing.T) {
			logout := 0
			upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/v1/login":
					http.SetCookie(w, &http.Cookie{Name: "openpak_session", Value: value})
					w.WriteHeader(tc.status)
					_, _ = io.WriteString(w, tc.body)
				case "/v1/logout":
					if r.Header.Get("Authorization") != "Bearer "+value {
						t.Error("missing private session")
					}
					logout++
					w.WriteHeader(204)
				default:
					t.Error("unexpected core request")
					w.WriteHeader(500)
				}
			}))
			defer upstream.Close()
			s := &service{usernameRealm: true, accountURL: upstream.URL, issuerURL: "https://own.test", client: upstream.Client(), pending: map[string]*challenge{"ABCD": {state: "pending", deviceKey: strings.Repeat("a", 64), expires: time.Now().Add(time.Minute)}}, grants: make(map[string]grant)}
			form := url.Values{"csrf_token": {"csrf"}, "user_code": {"ABCD"}, "decision": {tc.decision}, "username": {"tester"}, "password": {"test-password"}}
			req := httptest.NewRequest("POST", "https://own.test/device", strings.NewReader(form.Encode()))
			req.RemoteAddr = "198.51.100.10:1234"
			req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			req.Header.Set("Origin", "https://own.test")
			req.AddCookie(&http.Cookie{Name: "rc_device_form", Value: "csrf"})
			w := httptest.NewRecorder()
			s.approve(w, req)
			if w.Code != tc.want {
				t.Fatalf("status=%d want=%d", w.Code, tc.want)
			}
			if logout != 1 {
				t.Fatalf("temporary sessions cleaned=%d", logout)
			}
		})
	}
}
