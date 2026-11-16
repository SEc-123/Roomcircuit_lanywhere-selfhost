// SPDX-License-Identifier: AGPL-3.0-only
// A loopback-only development registration proxy. Not a public ingress.
package main

import (
	"bytes"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"time"
)

type gateway struct {
	upstream, origin, internalKey string
	client                        *http.Client
	slots                         chan struct{}
}

func localAddress(address string) bool {
	host, port, err := net.SplitHostPort(address)
	ip := net.ParseIP(host)
	return err == nil && ip != nil && ip.IsLoopback() && port != "" && port != "0"
}
func (g *gateway) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	peer, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || net.ParseIP(peer) == nil || !net.ParseIP(peer).IsLoopback() {
		http.Error(w, "forbidden", 403)
		return
	}
	if r.URL.Path != "/register" || r.URL.RawQuery != "" || (r.Method != "GET" && r.Method != "POST") {
		http.NotFound(w, r)
		return
	}
	if r.Header.Get("X-Internal-Key") != "" {
		http.Error(w, "forbidden", 403)
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" && subtle.ConstantTimeCompare([]byte(origin), []byte(g.origin)) != 1 {
		http.Error(w, "forbidden", 403)
		return
	}
	select {
	case g.slots <- struct{}{}:
		defer func() { <-g.slots }()
	default:
		http.Error(w, "busy", 429)
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 4096))
	if err != nil {
		http.Error(w, "bad request", 400)
		return
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, g.upstream+"/register", bytes.NewReader(body))
	if err != nil {
		http.Error(w, "unavailable", 503)
		return
	}
	req.Header.Set("X-Internal-Key", g.internalKey)
	if value := r.Header.Get("Cookie"); value != "" {
		req.Header.Set("Cookie", value)
	}
	if value := r.Header.Get("Content-Type"); value != "" {
		req.Header.Set("Content-Type", value)
	}
	if r.Method == "POST" {
		req.Header.Set("Origin", g.upstream)
	}
	res, err := g.client.Do(req)
	if err != nil {
		http.Error(w, "unavailable", 503)
		return
	}
	defer res.Body.Close()
	data, err := io.ReadAll(io.LimitReader(res.Body, 32769))
	if err != nil || len(data) > 32768 {
		http.Error(w, "unavailable", 503)
		return
	}
	for _, name := range []string{"Set-Cookie", "Content-Type", "Content-Security-Policy", "Referrer-Policy"} {
		for _, value := range res.Header.Values(name) {
			w.Header().Add(name, value)
		}
	}
	w.WriteHeader(res.StatusCode)
	_, _ = w.Write(data)
}
func parseConfig() (string, *gateway, error) {
	address, upstream, public, key := os.Getenv("RC_REGISTER_LISTEN"), os.Getenv("RC_REGISTER_UPSTREAM"), os.Getenv("RC_REGISTER_ORIGIN"), os.Getenv("RC_REGISTER_INTERNAL_KEY")
	if !localAddress(address) || len(key) < 32 {
		return "", nil, errors.New("registration gateway requires a private listener and key")
	}
	u, e := url.Parse(upstream)
	if e != nil || u.Scheme != "https" || !localAddress(u.Host) || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
		return "", nil, errors.New("broker must be a loopback HTTPS origin")
	}
	o, e := url.Parse(public)
	if e != nil || o.Scheme != "https" || o.Host != address || o.User != nil || o.Path != "" || o.RawQuery != "" || o.Fragment != "" {
		return "", nil, errors.New("gateway origin must match its loopback listener")
	}
	pem, e := os.ReadFile(os.Getenv("RC_REGISTER_CA"))
	if e != nil {
		return "", nil, errors.New("cannot read broker CA")
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(pem) {
		return "", nil, errors.New("invalid broker CA")
	}
	transport := &http.Transport{Proxy: nil, TLSClientConfig: &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12}, TLSHandshakeTimeout: 4 * time.Second, ResponseHeaderTimeout: 8 * time.Second, IdleConnTimeout: 30 * time.Second}
	client := &http.Client{Transport: transport, Timeout: 12 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	return address, &gateway{upstream: upstream, origin: public, internalKey: key, client: client, slots: make(chan struct{}, 32)}, nil
}
func main() {
	addr, g, err := parseConfig()
	if err != nil {
		log.Fatal(err)
	}
	server := http.Server{Addr: addr, Handler: g, ReadHeaderTimeout: 4 * time.Second, ReadTimeout: 8 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 8192, TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12}}
	log.Fatal(server.ListenAndServeTLS(os.Getenv("RC_REGISTER_CERT"), os.Getenv("RC_REGISTER_TLS_KEY")))
}
