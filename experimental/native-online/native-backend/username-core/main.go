// SPDX-License-Identifier: AGPL-3.0-only
// username-core is an independent account realm; it does not create email identities.
package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"golang.org/x/crypto/bcrypt"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

const namespace = "roomcircuit-lab"

var usernameRE = regexp.MustCompile(`^[a-z][a-z0-9_]{2,23}$`)

type core struct {
	accountv1.UnimplementedIdentityServer
	accountv1.UnimplementedLinksServer
	accountv1.UnimplementedEventsServer
	db            *pgxpool.Pool
	key           string
	dummy         []byte
	passwordSlots chan struct{}
}

func (c *core) acquirePassword() bool {
	select {
	case c.passwordSlots <- struct{}{}:
		return true
	default:
		return false
	}
}
func (c *core) releasePassword() { <-c.passwordSlots }

func id() string {
	var b [16]byte
	if _, e := rand.Read(b[:]); e != nil {
		panic(e)
	}
	b[6] = (b[6] & 15) | 64
	b[8] = (b[8] & 63) | 128
	h := hex.EncodeToString(b[:])
	return h[:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:]
}
func token() string {
	b := make([]byte, 32)
	if _, e := rand.Read(b); e != nil {
		panic(e)
	}
	return hex.EncodeToString(b)
}
func jsonOut(w http.ResponseWriter, n int, v any) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(n)
	_ = json.NewEncoder(w).Encode(v)
}
func parse(w http.ResponseWriter, r *http.Request, v any) bool {
	d := json.NewDecoder(http.MaxBytesReader(w, r.Body, 2048))
	d.DisallowUnknownFields()
	if d.Decode(v) != nil || !errors.Is(d.Decode(new(any)), io.EOF) {
		jsonOut(w, 400, map[string]string{"error": "invalid_request"})
		return false
	}
	return true
}
func normalize(s string) string { return strings.ToLower(strings.TrimSpace(s)) }

// Persistent, per-source and per-name limits apply before expensive password hashing.
func (c *core) limit(ctx context.Context, kind, key string, max int) bool {
	sum := sha256.Sum256([]byte(kind + ":" + key))
	var n int
	err := c.db.QueryRow(ctx, `INSERT INTO rc_user_rate(key,window_at,count) VALUES($1,now(),1)
 ON CONFLICT(key) DO UPDATE SET count=CASE WHEN rc_user_rate.window_at < now()-interval '15 minutes' THEN 1 ELSE rc_user_rate.count+1 END,
 window_at=CASE WHEN rc_user_rate.window_at < now()-interval '15 minutes' THEN now() ELSE rc_user_rate.window_at END RETURNING count`, sum[:]).Scan(&n)
	return err == nil && n <= max
}
func (c *core) register(w http.ResponseWriter, r *http.Request) {
	var x struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if !parse(w, r, &x) {
		return
	}
	name := normalize(x.Username)
	if !usernameRE.MatchString(name) || len(x.Password) < 8 || len(x.Password) > 72 {
		jsonOut(w, 400, map[string]string{"error": "invalid_credentials"})
		return
	}
	if !c.limit(r.Context(), "register-name", name, 8) {
		jsonOut(w, 429, map[string]string{"error": "rate_limited"})
		return
	}
	if !c.acquirePassword() {
		jsonOut(w, 429, map[string]string{"error": "rate_limited"})
		return
	}
	defer c.releasePassword()
	hash, e := bcrypt.GenerateFromPassword([]byte(x.Password), 12)
	if e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	_, e = c.db.Exec(r.Context(), `INSERT INTO rc_user_account(id,username,password_hash) VALUES($1,$2,$3)`, id(), name, string(hash))
	if e != nil {
		var exists bool
		check := c.db.QueryRow(r.Context(), `SELECT EXISTS(SELECT 1 FROM rc_user_account WHERE username=$1)`, name).Scan(&exists)
		if check == nil && exists {
			jsonOut(w, 409, map[string]string{"error": "username_taken"})
		} else {
			jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		}
		return
	}
	jsonOut(w, 201, map[string]string{"status": "created"})
}
func (c *core) login(w http.ResponseWriter, r *http.Request) {
	var x struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if !parse(w, r, &x) {
		return
	}
	name := normalize(x.Username)
	if !usernameRE.MatchString(name) || len(x.Password) > 72 {
		jsonOut(w, 401, map[string]string{"error": "invalid_credentials"})
		return
	}
	if !c.limit(r.Context(), "login-name", name, 12) {
		jsonOut(w, 429, map[string]string{"error": "rate_limited"})
		return
	}
	if !c.acquirePassword() {
		jsonOut(w, 429, map[string]string{"error": "rate_limited"})
		return
	}
	defer c.releasePassword()
	var account, hash, state string
	e := c.db.QueryRow(r.Context(), `SELECT id,password_hash,state FROM rc_user_account WHERE username=$1`, name).Scan(&account, &hash, &state)
	if e != nil {
		_ = bcrypt.CompareHashAndPassword(c.dummy, []byte(x.Password))
		if !errors.Is(e, pgx.ErrNoRows) {
			jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
			return
		}
		jsonOut(w, 401, map[string]string{"error": "invalid_credentials"})
		return
	}
	if bcrypt.CompareHashAndPassword([]byte(hash), []byte(x.Password)) != nil || state != "active" {
		jsonOut(w, 401, map[string]string{"error": "invalid_credentials"})
		return
	}
	raw := token()
	digest := sha256.Sum256([]byte(raw))
	tx, e := c.db.Begin(r.Context())
	if e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	defer tx.Rollback(r.Context())
	if e = tx.QueryRow(r.Context(), `SELECT state FROM rc_user_account WHERE id=$1 FOR UPDATE`, account).Scan(&state); e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	if state != "active" {
		jsonOut(w, 401, map[string]string{"error": "invalid_credentials"})
		return
	}
	if _, e = tx.Exec(r.Context(), `DELETE FROM rc_user_session WHERE account_id=$1 AND expires<=now()`, account); e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	var active int
	if e = tx.QueryRow(r.Context(), `SELECT count(*) FROM rc_user_session WHERE account_id=$1 AND expires>now()`, account).Scan(&active); e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	if active >= 20 {
		jsonOut(w, 429, map[string]string{"error": "session_limit"})
		return
	}
	if _, e = tx.Exec(r.Context(), `INSERT INTO rc_user_session(token_hash,account_id,expires) VALUES($1,$2,now()+interval '2 hours')`, digest[:], account); e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	if e = tx.Commit(r.Context()); e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	http.SetCookie(w, &http.Cookie{Name: "openpak_session", Value: raw, Path: "/", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode, MaxAge: 7200})
	jsonOut(w, 200, map[string]string{"account_id": account})
}
func (c *core) whoami(w http.ResponseWriter, r *http.Request) {
	val := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	if val == "" || val == r.Header.Get("Authorization") {
		if cookie, e := r.Cookie("openpak_session"); e == nil {
			val = cookie.Value
		}
	}
	if len(val) != 64 {
		jsonOut(w, 401, map[string]string{"error": "unauthorized"})
		return
	}
	digest := sha256.Sum256([]byte(val))
	var account, name, state string
	e := c.db.QueryRow(r.Context(), `SELECT a.id,a.username,a.state FROM rc_user_session s JOIN rc_user_account a ON a.id=s.account_id WHERE s.token_hash=$1 AND s.expires>now()`, digest[:]).Scan(&account, &name, &state)
	if errors.Is(e, pgx.ErrNoRows) {
		jsonOut(w, 401, map[string]string{"error": "unauthorized"})
		return
	}
	if e != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	if state != "active" {
		jsonOut(w, 401, map[string]string{"error": "unauthorized"})
		return
	}
	jsonOut(w, 200, map[string]any{"account_id": account, "username": name, "status": "active", "account_type": "username", "email_verified": false})
}

func (c *core) logout(w http.ResponseWriter, r *http.Request) {
	val := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	if val == "" || val == r.Header.Get("Authorization") {
		if cookie, err := r.Cookie("openpak_session"); err == nil {
			val = cookie.Value
		}
	}
	if len(val) != 64 {
		jsonOut(w, 401, map[string]string{"error": "unauthorized"})
		return
	}
	digest := sha256.Sum256([]byte(val))
	if _, err := c.db.Exec(r.Context(), `DELETE FROM rc_user_session WHERE token_hash=$1`, digest[:]); err != nil {
		jsonOut(w, 503, map[string]string{"error": "service_unavailable"})
		return
	}
	http.SetCookie(w, &http.Cookie{Name: "openpak_session", Value: "", Path: "/", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode, MaxAge: -1})
	w.WriteHeader(http.StatusNoContent)
}
func internal(key string) grpc.UnaryServerInterceptor {
	return func(ctx context.Context, req any, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (any, error) {
		m, _ := metadata.FromIncomingContext(ctx)
		v := m.Get("x-internal-key")
		if len(v) != 1 || subtle.ConstantTimeCompare([]byte(v[0]), []byte(key)) != 1 {
			return nil, status.Error(codes.Unauthenticated, "unauthorized")
		}
		return handler(ctx, req)
	}
}
func loopback(v string) bool {
	h, _, e := net.SplitHostPort(v)
	return e == nil && net.ParseIP(h).IsLoopback()
}
func main() {
	ctx := context.Background()
	dsn, key := os.Getenv("RC_USERNAME_DATABASE_URL"), os.Getenv("RC_USERNAME_CORE_KEY")
	httpAddr, grpcAddr := os.Getenv("RC_USERNAME_HTTP"), os.Getenv("RC_USERNAME_GRPC")
	if dsn == "" || len(key) < 32 || !loopback(httpAddr) || !loopback(grpcAddr) {
		log.Fatal("username core requires private listeners and configuration")
	}
	config, e := pgxpool.ParseConfig(dsn)
	if e != nil {
		log.Fatal("invalid database configuration")
	}
	config.MaxConns = 10
	db, e := pgxpool.NewWithConfig(ctx, config)
	if e != nil {
		log.Fatal(e)
	}
	defer db.Close()
	if _, e = db.Exec(ctx, schema); e != nil {
		log.Fatal(e)
	}
	dummy, e := bcrypt.GenerateFromPassword([]byte(token()), 12)
	if e != nil {
		log.Fatal(e)
	}
	c := &core{db: db, key: key, dummy: dummy, passwordSlots: make(chan struct{}, 4)}
	server := grpc.NewServer(grpc.UnaryInterceptor(internal(key)))
	accountv1.RegisterIdentityServer(server, c)
	accountv1.RegisterLinksServer(server, c)
	accountv1.RegisterEventsServer(server, c)
	listener, e := net.Listen("tcp", grpcAddr)
	if e != nil {
		log.Fatal(e)
	}
	go func() { log.Fatal(server.Serve(listener)) }()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/register", c.register)
	mux.HandleFunc("POST /v1/login", c.login)
	mux.HandleFunc("POST /v1/logout", c.logout)
	mux.HandleFunc("GET /v1/whoami", c.whoami)
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { jsonOut(w, 200, map[string]string{"status": "ok"}) })
	mux.HandleFunc("GET /internal/realm", func(w http.ResponseWriter, r *http.Request) {
		if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Internal-Key")), []byte(key)) != 1 {
			jsonOut(w, 401, map[string]string{"error": "unauthorized"})
			return
		}
		jsonOut(w, 200, map[string]string{"realm": "roomcircuit-username-v1"})
	})
	go func() {
		for range time.NewTicker(time.Hour).C {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_, _ = db.Exec(ctx, `DELETE FROM rc_user_session WHERE expires < now()`)
			_, _ = db.Exec(ctx, `DELETE FROM rc_user_rate WHERE window_at < now()-interval '1 hour'`)
			cancel()
		}
	}()
	h := http.Server{Addr: httpAddr, Handler: mux, ReadHeaderTimeout: 4 * time.Second, ReadTimeout: 8 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 8192}
	fmt.Printf("username core ready\n")
	log.Fatal(h.ListenAndServe())
}
