package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestGatewayKeepsInternalKeyOnLoopbackOnly(t *testing.T) {
	key := strings.Repeat("k", 64)
	calls := 0
	var upstream *httptest.Server
	upstream = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Header.Get("X-Internal-Key") != key || r.Header.Get("X-Forwarded-For") != "" {
			t.Error("wrong internal trust boundary")
		}
		if r.Method == "POST" && r.Header.Get("Origin") != upstream.URL {
			t.Error("wrong upstream origin")
		}
		if r.Method == "GET" {
			http.SetCookie(w, &http.Cookie{Name: "csrf", Value: "token", Path: "/register", HttpOnly: true, Secure: true})
		}
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, "ok")
	}))
	defer upstream.Close()
	g := gateway{upstream: upstream.URL, origin: "https://127.0.0.1:39917", internalKey: key, slots: make(chan struct{}, 2), client: upstream.Client()}
	req := func(method, path, origin, peer string, forged bool) *httptest.ResponseRecorder {
		r := httptest.NewRequest(method, "https://127.0.0.1:39917"+path, strings.NewReader("a=b"))
		r.RemoteAddr = peer
		r.Header.Set("Origin", origin)
		r.Header.Set("X-Forwarded-For", "203.0.113.9")
		if forged {
			r.Header.Set("X-Internal-Key", "forged")
		}
		w := httptest.NewRecorder()
		g.ServeHTTP(w, r)
		return w
	}
	get := req("GET", "/register", "", "127.0.0.1:3210", false)
	if get.Code != 200 || get.Header().Get("Set-Cookie") == "" {
		t.Fatal("registration form or cookie not forwarded")
	}
	before := calls
	for _, r := range []*httptest.ResponseRecorder{req("GET", "/register", "", "198.51.100.1:3210", false), req("POST", "/register", "https://foreign.example", "127.0.0.1:3210", false), req("POST", "/register", "https://127.0.0.1:39917", "127.0.0.1:3210", true), req("GET", "/device", "", "127.0.0.1:3210", false)} {
		if r.Code < 400 {
			t.Fatal("rejected request unexpectedly passed")
		}
	}
	if calls != before {
		t.Fatal("rejected request reached upstream")
	}
	post := req("POST", "/register", "https://127.0.0.1:39917", "127.0.0.1:3210", false)
	if post.Code != 200 {
		t.Fatal("local registration failed")
	}
}
