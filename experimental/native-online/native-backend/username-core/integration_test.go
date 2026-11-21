package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"github.com/jackc/pgx/v5/pgxpool"
	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"golang.org/x/crypto/bcrypt"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

func TestRealPostgresUsernameLifecycle(t *testing.T) {
	dsn := os.Getenv("RC_USERNAME_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("disposable Postgres URL not provided")
	}
	ctx := context.Background()
	db, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err = db.Exec(ctx, schema); err != nil {
		t.Fatal(err)
	}
	dummy, _ := bcrypt.GenerateFromPassword([]byte("never-used-dummy"), 12)
	c := &core{db: db, key: strings.Repeat("k", 32), dummy: dummy, passwordSlots: make(chan struct{}, 4)}
	name := fmt.Sprintf("p%d", time.Now().UnixNano())
	password := "valid-password-test"
	body := func(v any) []byte { b, _ := json.Marshal(v); return b }
	request := func(fn http.HandlerFunc, m, p string, payload []byte, cookie *http.Cookie) *httptest.ResponseRecorder {
		r := httptest.NewRequest(m, p, bytes.NewReader(payload))
		if cookie != nil {
			r.AddCookie(cookie)
		}
		w := httptest.NewRecorder()
		fn(w, r)
		return w
	}
	creds := map[string]string{"username": name, "password": password}
	w := request(c.register, "POST", "/v1/register", body(creds), nil)
	if w.Code != 201 {
		t.Fatalf("register status %d", w.Code)
	}
	w = request(c.register, "POST", "/v1/register", body(creds), nil)
	if w.Code != 409 {
		t.Fatalf("duplicate status %d", w.Code)
	}
	w = request(c.login, "POST", "/v1/login", body(map[string]string{"username": name, "password": "wrong-password"}), nil)
	if w.Code != 401 {
		t.Fatalf("invalid login status %d", w.Code)
	}
	w = request(c.login, "POST", "/v1/login", body(creds), nil)
	if w.Code != 200 {
		t.Fatalf("login status %d", w.Code)
	}
	var login struct {
		Account string `json:"account_id"`
	}
	if json.Unmarshal(w.Body.Bytes(), &login) != nil || len(login.Account) != 36 {
		t.Fatal("missing account")
	}
	cookie := w.Result().Cookies()[0]
	w = request(c.whoami, "GET", "/v1/whoami", nil, cookie)
	var who map[string]any
	_ = json.Unmarshal(w.Body.Bytes(), &who)
	if w.Code != 200 || who["account_type"] != "username" || who["email_verified"] != false {
		t.Fatalf("incorrect identity %d %#v", w.Code, who)
	}
	profile, err := c.GetAccount(ctx, &accountv1.GetAccountRequest{AccountId: login.Account})
	if err != nil || profile.Email != "" || profile.EmailVerified || profile.Handle != name {
		t.Fatal("email-free profile failed")
	}
	subject := strings.Repeat("ab", 32)
	link, err := c.ReserveLink(ctx, &accountv1.ReserveLinkRequest{AccountId: login.Account, Namespace: namespace, SubjectId: subject})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = c.ActivateLink(ctx, &accountv1.ActivateLinkRequest{LinkId: link.LinkId}); err != nil {
		t.Fatal(err)
	}
	if _, err = c.GetLinkBySubject(ctx, &accountv1.GetLinkBySubjectRequest{Namespace: namespace, SubjectId: subject}); err != nil {
		t.Fatal(err)
	}
	if _, err = c.UnlinkLink(ctx, &accountv1.UnlinkLinkRequest{LinkId: link.LinkId}); err != nil {
		t.Fatal(err)
	}
	if _, err = c.GetLinkBySubject(ctx, &accountv1.GetLinkBySubjectRequest{Namespace: namespace, SubjectId: subject}); status.Code(err) != codes.NotFound {
		t.Fatal("unlinked subject remained active")
	}
	ev, err := c.PollEvents(ctx, &accountv1.PollEventsRequest{SinceVersion: 0, Limit: 1000})
	if err != nil || len(ev.Events) != 1 || ev.Events[0].Type != "link_unlinked" {
		t.Fatal("unlink event missing")
	}
	// A fresh service instance uses the persisted session and revoked state.
	c = &core{db: db, key: strings.Repeat("k", 32), dummy: dummy, passwordSlots: make(chan struct{}, 4)}
	if w = request(c.whoami, "GET", "/v1/whoami", nil, cookie); w.Code != 200 {
		t.Fatal("session lost after restart")
	}
	if _, err = c.GetLinkBySubject(ctx, &accountv1.GetLinkBySubjectRequest{Namespace: namespace, SubjectId: subject}); status.Code(err) != codes.NotFound {
		t.Fatal("unlink lost after restart")
	}
	// Distinct accounts racing for one subject must not both obtain it.
	second := "q" + name[1:]
	secondCreds := map[string]string{"username": second, "password": password}
	if w = request(c.register, "POST", "/v1/register", body(secondCreds), nil); w.Code != 201 {
		t.Fatalf("second registration status %d", w.Code)
	}
	w = request(c.login, "POST", "/v1/login", body(secondCreds), nil)
	var secondLogin struct {
		Account string `json:"account_id"`
	}
	if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &secondLogin) != nil {
		t.Fatal("second login failed")
	}
	competing := strings.Repeat("cd", 32)
	results := make(chan error, 2)
	for _, account := range []string{login.Account, secondLogin.Account} {
		go func(owner string) {
			_, err := c.ReserveLink(ctx, &accountv1.ReserveLinkRequest{AccountId: owner, Namespace: namespace, SubjectId: competing})
			results <- err
		}(account)
	}
	one, two := <-results, <-results
	if !((one == nil && status.Code(two) == codes.AlreadyExists) || (two == nil && status.Code(one) == codes.AlreadyExists)) {
		t.Fatalf("concurrent ownership results: %v, %v", one, two)
	}
	// Cancelling a transaction before unlink must preserve the active link and event count.
	reserved, err := c.ReserveLink(ctx, &accountv1.ReserveLinkRequest{AccountId: login.Account, Namespace: namespace, SubjectId: subject})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = c.ActivateLink(ctx, &accountv1.ActivateLinkRequest{LinkId: reserved.LinkId}); err != nil {
		t.Fatal(err)
	}
	stopped, cancel := context.WithCancel(ctx)
	cancel()
	if _, err = c.UnlinkLink(stopped, &accountv1.UnlinkLinkRequest{LinkId: reserved.LinkId}); err == nil {
		t.Fatal("cancelled unlink succeeded")
	}
	if _, err = c.GetLinkBySubject(ctx, &accountv1.GetLinkBySubjectRequest{Namespace: namespace, SubjectId: subject}); err != nil {
		t.Fatal("cancelled unlink changed active link")
	}
	after, err := c.PollEvents(ctx, &accountv1.PollEventsRequest{SinceVersion: ev.MaxVersion, Limit: 1000})
	if err != nil || len(after.Events) != 0 {
		t.Fatal("cancelled unlink emitted event")
	}
	if w = request(c.logout, "POST", "/v1/logout", nil, cookie); w.Code != http.StatusNoContent {
		t.Fatal("logout failed")
	}
	if w = request(c.whoami, "GET", "/v1/whoami", nil, cookie); w.Code != http.StatusUnauthorized {
		t.Fatal("logged out session remained valid")
	}
}
