// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"context"
	"encoding/json"
	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"google.golang.org/grpc/metadata"
	"html/template"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

type edgeBucket struct {
	start time.Time
	count int
}
type edgeLimiter struct {
	mu                   sync.Mutex
	items                map[string]edgeBucket
	allowLoopbackForTest bool
}

func (l *edgeLimiter) allow(r *http.Request, action string, max int) bool {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || net.ParseIP(host) == nil {
		return false
	}
	// Do not trust forwarded headers. Run this listener directly or configure an
	// audited trusted proxy at the edge; shared loopback is not a client address.
	if net.ParseIP(host).IsLoopback() && !l.allowLoopbackForTest {
		return false
	}
	key := action + ":" + host
	now := time.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.items == nil {
		l.items = make(map[string]edgeBucket)
	}
	for k, v := range l.items {
		if now.Sub(v.start) >= 15*time.Minute {
			delete(l.items, k)
		}
	}
	v, exists := l.items[key]
	if !exists {
		if len(l.items) >= 4096 {
			return false
		}
		v = edgeBucket{start: now}
	}
	v.count++
	l.items[key] = v
	return v.count <= max
}

var usernameRegisterPage = template.Must(template.New("register").Parse(`<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Create RoomCircuit account</title><main><h1>Create account</h1><form method="post" action="/register"><input type="hidden" name="csrf" value="{{.CSRF}}"><label>Username <input name="username" pattern="[A-Za-z][A-Za-z0-9_]{2,23}" required></label><br><label>Password <input name="password" type="password" minlength="8" maxlength="72" required></label><br><button>Create account</button></form><p>Keep your password safe; this account has no email recovery.</p></main></html>`))

func (s *service) usernamePage(w http.ResponseWriter, _ *http.Request) {
	csrf := randomHex(24)
	http.SetCookie(w, &http.Cookie{Name: "rc_register", Value: csrf, Path: "/register", Secure: true, HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: 300})
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
	_ = usernameRegisterPage.Execute(w, map[string]string{"CSRF": csrf})
}
func (s *service) usernameRegister(w http.ResponseWriter, r *http.Request) {
	if !s.trustedInternal(r) && !s.edge.allow(r, "register", 20) {
		reject(w, 429, "rate_limited")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 2048)
	if r.ParseForm() != nil {
		reject(w, 400, "invalid_request")
		return
	}
	cookie, err := r.Cookie("rc_register")
	if err != nil || !equal(cookie.Value, r.PostForm.Get("csrf")) || (r.Header.Get("Origin") != "" && r.Header.Get("Origin") != s.issuerURL) {
		reject(w, 403, "invalid_browser_session")
		return
	}
	username, password := strings.TrimSpace(r.PostForm.Get("username")), r.PostForm.Get("password")
	if username == "" || len(username) > 24 || len(password) < 8 || len(password) > 72 {
		reject(w, 400, "invalid_request")
		return
	}
	res, err := s.relay(r, s.accountURL+"/v1/register", map[string]string{"username": username, "password": password}, "", "")
	if err != nil {
		reject(w, 503, "account_unavailable")
		return
	}
	defer res.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(res.Body, 4096))
	if res.StatusCode == 409 {
		reject(w, 409, "username_taken")
		return
	}
	if res.StatusCode == 400 {
		reject(w, 400, "invalid_request")
		return
	}
	if res.StatusCode != 201 {
		reject(w, 503, "account_unavailable")
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = io.WriteString(w, "<!doctype html><html lang=\"en\"><title>Account created</title><main><h1>Account created</h1><p>Return to device linking and sign in.</p></main></html>")
}
func (s *service) validateUsernameRealm() error {
	req, err := http.NewRequest("GET", s.accountURL+"/internal/realm", nil)
	if err != nil {
		return err
	}
	req.Header.Set("X-Internal-Key", s.authority.key)
	res, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	var v struct {
		Realm string `json:"realm"`
	}
	if res.StatusCode != 200 || json.NewDecoder(io.LimitReader(res.Body, 1024)).Decode(&v) != nil || v.Realm != "roomcircuit-username-v1" {
		return io.ErrUnexpectedEOF
	}
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	ctx = metadata.AppendToOutgoingContext(ctx, "x-internal-key", s.authority.key)
	marker, err := s.authority.accounts.GetAccount(ctx, &accountv1.GetAccountRequest{AccountId: "roomcircuit-username-realm-v1"})
	if err != nil || marker.GetAccountId() != "roomcircuit-username-realm-v1" || marker.GetDisplayName() != "roomcircuit-username-v1" {
		return io.ErrUnexpectedEOF
	}
	return nil
}

func (s *service) trustedInternal(r *http.Request) bool {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	return err == nil && net.ParseIP(host).IsLoopback() && len(s.gameKey) >= 32 && equal(r.Header.Get("X-Internal-Key"), s.gameKey)
}
