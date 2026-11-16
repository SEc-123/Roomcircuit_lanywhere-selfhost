package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"maps"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unicode/utf8"

	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

const ownLinkNamespace = "roomcircuit-lab"
const refreshTTL = 30 * 24 * time.Hour
const maxDurableGrants = 4096

type approvedDevice struct {
	Account       string    `json:"account"`
	Subject       string    `json:"subject"`
	PID           uint64    `json:"pid"`
	LinkVersion   uint64    `json:"link_version,omitempty"`
	RefreshSHA256 [32]byte  `json:"refresh_sha256"`
	RefreshUntil  time.Time `json:"refresh_until"`
}

type accountAuthority struct {
	key      string
	links    accountv1.LinksClient
	accounts accountv1.IdentityClient
	events   accountv1.EventsClient
}

type durableGrants struct {
	path          string
	lock          *os.File
	active        map[string]approvedDevice
	cursor        uint64
	revoked       map[string]uint64
	syncDirectory func(*os.File) error
}

type persistedGrants struct {
	Format  int                       `json:"format"`
	Active  map[string]approvedDevice `json:"active"`
	Cursor  uint64                    `json:"events_cursor"`
	Revoked map[string]uint64         `json:"link_revocations"`
}

func coreAuthority(addr, key string) (*accountAuthority, error) {
	host, _, err := net.SplitHostPort(addr)
	if err != nil || !net.ParseIP(host).IsLoopback() || len(key) < 32 {
		return nil, errors.New("account grant verification requires private loopback gRPC")
	}
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, err
	}
	return &accountAuthority{key, accountv1.NewLinksClient(conn), accountv1.NewIdentityClient(conn), accountv1.NewEventsClient(conn)}, nil
}

func (a *accountAuthority) verify(ctx context.Context, item approvedDevice) (*accountv1.GetAccountResponse, int, bool) {
	ctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	ctx = metadata.AppendToOutgoingContext(ctx, "x-internal-key", a.key)
	link, err := a.links.GetLinkBySubject(ctx, &accountv1.GetLinkBySubjectRequest{Namespace: ownLinkNamespace, SubjectId: item.Subject})
	if status.Code(err) == codes.NotFound || (err == nil && (link.GetAccountId() != item.Account || link.GetState() != accountv1.LinkState_LINK_STATE_ACTIVE)) {
		return nil, 403, true
	}
	if err != nil {
		return nil, 503, false
	}
	profile, err := a.accounts.GetAccount(ctx, &accountv1.GetAccountRequest{AccountId: item.Account})
	if status.Code(err) == codes.NotFound || (err == nil && (profile.GetStatus() != accountv1.AccountStatus_ACCOUNT_STATUS_ACTIVE || !profile.GetEmailVerified())) {
		return nil, 403, false
	}
	if err != nil {
		return nil, 503, false
	}
	return profile, 200, false
}

func openDurableGrants(path string) (*durableGrants, error) {
	if path == "" {
		return nil, errors.New("RC_DEVICE_GRANT_STATE required")
	}
	lock, err := os.OpenFile(path+".lock", os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		lock.Close()
		return nil, err
	}
	s := &durableGrants{path: path, lock: lock, active: make(map[string]approvedDevice), revoked: make(map[string]uint64), syncDirectory: (*os.File).Sync}
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return s, nil
	}
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0o077 != 0 || info.Size() > 8<<20 {
		lock.Close()
		return nil, errors.New("grant state must be a bounded, owner-only file")
	}
	file, err := os.Open(path)
	if err != nil {
		lock.Close()
		return nil, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, 8<<20))
	if err != nil || s.decode(data) != nil {
		lock.Close()
		return nil, errors.New("invalid durable grant state")
	}
	for id, v := range s.active {
		entry, e := hex.DecodeString(id)
		subject, se := hex.DecodeString(v.Subject)
		if e != nil || len(entry) != 24 || se != nil || len(subject) != 32 || v.PID == 0 || v.Account == "" || v.LinkVersion > s.cursor || v.RefreshSHA256 == ([32]byte{}) || v.RefreshUntil.IsZero() {
			lock.Close()
			return nil, errors.New("invalid durable device grant")
		}
	}
	return s, nil
}

// Called with service.mu held. After rename, memory follows the committed file.
func (s *durableGrants) save(next persistedGrants) (committed bool, err error) {
	dir, err := os.Open(filepath.Dir(s.path))
	if err != nil {
		return false, err
	}
	defer dir.Close()
	file, err := os.CreateTemp(filepath.Dir(s.path), ".approved-grants-")
	if err != nil {
		return false, err
	}
	defer os.Remove(file.Name())
	defer file.Close()
	if err = json.NewEncoder(file).Encode(next); err != nil {
		return false, err
	}
	if err = file.Sync(); err != nil {
		return false, err
	}
	if err = file.Close(); err != nil {
		return false, err
	}
	if err = os.Rename(file.Name(), s.path); err != nil {
		return false, err
	}
	s.active, s.cursor, s.revoked = next.Active, next.Cursor, next.Revoked
	return true, s.syncDirectory(dir)
}

func (s *durableGrants) commit(next map[string]approvedDevice) bool {
	return s.commitState(persistedGrants{Format: 1, Active: next, Cursor: s.cursor, Revoked: s.revoked})
}

func (s *durableGrants) commitState(next persistedGrants) bool {
	committed, err := s.save(next)
	if err != nil {
		log.Printf("account grants saved=%t: %v", committed, err)
	}
	return committed
}

func (s *service) liveGrantAccount(ctx context.Context, id string, item approvedDevice, application string) (*accountv1.GetAccountResponse, int) {
	if !s.supportedApplication(application) {
		return nil, 403
	}
	value, status, unlinked := s.authority.verify(ctx, item)
	if unlinked {
		s.mu.Lock()
		if s.durable.active[id] == item {
			next := maps.Clone(s.durable.active)
			delete(next, id)
			s.durable.commit(next)
		}
		s.mu.Unlock()
	}
	if status != 200 {
		return nil, status
	}
	s.mu.Lock()
	eventErr := s.syncRevocations(ctx)
	current, exists := s.durable.active[id]
	s.mu.Unlock()
	if eventErr != nil {
		return nil, 503
	}
	if !exists || current.Account != item.Account || current.Subject != item.Subject || current.PID != item.PID || current.LinkVersion != item.LinkVersion {
		return nil, 403
	}
	return value, 200
}

func (s *service) liveGrant(ctx context.Context, id string, item approvedDevice, application string) (map[string]any, int) {
	profile, status := s.liveGrantAccount(ctx, id, item, application)
	if status != 200 {
		return nil, status
	}
	value := map[string]any{"account_id": item.Account, "pid": item.PID, "baas_user_id": fmt.Sprintf("%016x", item.PID),
		"nickname": profile.GetDisplayName(), "country": profile.GetCountry(), "friends": []string{}, "grant_id": id}
	if application != "" {
		value["application_id"] = application
	}
	return value, 200
}

func (s *service) signedPair(id string, item approvedDevice, application string, now time.Time) (claims, string, string, error) {
	identity := claims{Issuer: s.issuerURL, Audience: scope, Purpose: "id_token", Subject: fmt.Sprintf("%016x", item.PID),
		ID: randomHex(24), Grant: id, Application: application, Expires: now.Add(tokenTTL).Unix()}
	jwt, err := s.sign(identity)
	if err != nil {
		return claims{}, "", "", err
	}
	accessClaims := identity
	accessClaims.Purpose = "token"
	access, err := s.sign(accessClaims)
	return identity, jwt, access, err
}

func returnCredentials(w http.ResponseWriter, item approvedDevice, jwt, access, refresh string) {
	response(w, 200, map[string]any{"access_token": access, "id_token": jwt, "refresh_token": refresh,
		"token_type": "Bearer", "scope": scope, "expires_in": int(tokenTTL / time.Second), "pid": item.PID})
}

func (s *service) renewal(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Token       string `json:"refresh_token"`
		Application string `json:"application_id"`
	}
	if !request(w, r, &input) {
		return
	}
	if !s.supportedApplication(input.Application) {
		reject(w, 403, "wrong_application")
		return
	}
	decoded, err := hex.DecodeString(input.Token)
	if err != nil || len(decoded) != 32 {
		reject(w, 401, "invalid_grant")
		return
	}
	hash := sha256.Sum256([]byte(input.Token))
	s.mu.Lock()
	var id string
	var item approvedDevice
	for k, value := range s.durable.active {
		if value.RefreshSHA256 == hash {
			id, item = k, value
			break
		}
	}
	s.mu.Unlock()
	logout := r.URL.Path == "/v1/device/logout"
	if id == "" || (!logout && !time.Now().Before(item.RefreshUntil)) {
		reject(w, 401, "invalid_grant")
		return
	}
	if !logout {
		if _, status := s.liveGrant(r.Context(), id, item, input.Application); status != 200 {
			reject(w, status, "linked_account_unavailable")
			return
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	current, exists := s.durable.active[id]
	if !exists || current != item || (!logout && !time.Now().Before(item.RefreshUntil)) {
		reject(w, 401, "invalid_grant")
		return
	}
	next := maps.Clone(s.durable.active)
	if logout {
		delete(next, id)
		if !s.durable.commit(next) {
			reject(w, 503, "grant_store_unavailable")
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(204)
		return
	}
	replacement := randomHex(32)
	item.RefreshSHA256 = sha256.Sum256([]byte(replacement))
	identity, jwt, access, err := s.signedPair(id, item, input.Application, time.Now())
	if err != nil {
		reject(w, 503, "issuer_unavailable")
		return
	}
	next[id] = item
	if !s.durable.commit(next) {
		reject(w, 503, "grant_store_unavailable")
		return
	}
	s.rememberIdentity(identity, item.PID)
	returnCredentials(w, item, jwt, access, replacement)
}

func (s *service) revalidateGrant(w http.ResponseWriter, r *http.Request) {
	if !equal(r.Header.Get("X-Internal-Key"), s.gameKey) {
		reject(w, 401, "invalid_internal_key")
		return
	}
	var input struct {
		ID          string `json:"grant_id"`
		Application string `json:"application_id"`
	}
	if !request(w, r, &input) {
		return
	}
	s.mu.Lock()
	item, exists := s.durable.active[input.ID]
	s.mu.Unlock()
	if !exists {
		reject(w, 404, "unknown_grant")
		return
	}
	value, status := s.liveGrant(r.Context(), input.ID, item, input.Application)
	if status != 200 {
		reject(w, status, "linked_account_unavailable")
		return
	}
	response(w, 200, value)
}

func (s *service) accountProfile(w http.ResponseWriter, r *http.Request) {
	if !equal(r.Header.Get("X-Internal-Key"), s.gameKey) {
		reject(w, 401, "invalid_internal_key")
		return
	}
	var input struct {
		Token       string `json:"access_token"`
		Grant       string `json:"grant_id"`
		Application string `json:"application_id"`
	}
	if !request(w, r, &input) {
		return
	}
	if (input.Token == "") == (input.Grant == "") {
		reject(w, 401, "invalid_token")
		return
	}
	grantID, subject := input.Grant, ""
	if input.Token != "" {
		c, valid := s.verifyPurpose(input.Token, "token", time.Now())
		if !valid {
			reject(w, 401, "invalid_token")
			return
		}
		if c.Application != input.Application {
			reject(w, 403, "wrong_application")
			return
		}
		grantID, subject = c.Grant, c.Subject
	}
	s.mu.Lock()
	item, exists := s.durable.active[grantID]
	s.mu.Unlock()
	if !exists || (subject != "" && fmt.Sprintf("%016x", item.PID) != subject) {
		reject(w, 403, "linked_account_unavailable")
		return
	}
	profile, status := s.liveGrantAccount(r.Context(), grantID, item, input.Application)
	if status != 200 {
		reject(w, status, "linked_account_unavailable")
		return
	}
	nickname := strings.TrimSpace(profile.GetDisplayName())
	for len(nickname) > 32 {
		_, size := utf8.DecodeLastRuneInString(nickname)
		nickname = nickname[:len(nickname)-size]
	}
	_, birthdayErr := time.Parse("2006-01-02", profile.GetBirthday())
	if nickname == "" || len(profile.GetCountry()) != 2 || birthdayErr != nil || profile.GetEmail() == "" {
		reject(w, http.StatusConflict, "complete_account_profile")
		return
	}
	response(w, 200, map[string]any{"id": fmt.Sprintf("%016x", item.PID), "nickname": nickname, "email": profile.GetEmail(),
		"emailVerified": true, "country": profile.GetCountry(), "birthday": profile.GetBirthday(),
		"termsAgreementRequired": false, "summary": map[string]any{"nintendo": map[string]bool{"hasMembership": true}}})
}
