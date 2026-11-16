// The RoomCircuit account lab only issues identities for its own account core.
package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

const namespace = "roomcircuit-lab"
const ticketLifetime = 30 * time.Minute

type ticket struct {
	account string
	subject string
	pid     uint64
	expires time.Time
}

type issuer struct {
	coreURL string
	coreKey string
	gameKey string
	client  *http.Client
	links   accountv1.LinksClient
	users   accountv1.IdentityClient
	mu      sync.Mutex
	tickets map[[32]byte]ticket
}

type gameIdentity struct {
	PID       uint64   `json:"pid"`
	UserID    string   `json:"baas_user_id"`
	Nickname  string   `json:"nickname"`
	Country   string   `json:"country"`
	AccountID string   `json:"account_id"`
	Friends   []string `json:"friends"`
}

func jsonResponse(w http.ResponseWriter, code int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(value)
}

func deny(w http.ResponseWriter, code int) {
	jsonResponse(w, code, map[string]string{"error": http.StatusText(code)})
}

func readJSON(w http.ResponseWriter, r *http.Request, input any) bool {
	defer r.Body.Close()
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 2048))
	decoder.DisallowUnknownFields()
	if decoder.Decode(input) != nil || !errors.Is(decoder.Decode(new(any)), io.EOF) {
		deny(w, http.StatusBadRequest)
		return false
	}
	return true
}

func (s *issuer) core(ctx context.Context) context.Context {
	return metadata.AppendToOutgoingContext(ctx, "x-internal-key", s.coreKey)
}

func (s *issuer) verifiedAccount(r *http.Request) (string, int) {
	auth := r.Header.Get("Authorization")
	if !strings.HasPrefix(auth, "Bearer ") || len(auth) == 7 || len(auth) > 512 {
		return "", http.StatusUnauthorized
	}
	req, err := http.NewRequestWithContext(r.Context(), "GET", s.coreURL+"/v1/whoami", nil)
	if err != nil {
		return "", http.StatusServiceUnavailable
	}
	req.Header.Set("Authorization", auth)
	response, err := s.client.Do(req)
	if err != nil {
		return "", http.StatusServiceUnavailable
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusUnauthorized {
		return "", http.StatusUnauthorized
	}
	var profile struct {
		Account  string `json:"account_id"`
		Status   string `json:"status"`
		Verified bool   `json:"email_verified"`
	}
	if response.StatusCode != http.StatusOK || json.NewDecoder(io.LimitReader(response.Body, 4096)).Decode(&profile) != nil || profile.Account == "" {
		return "", http.StatusServiceUnavailable
	}
	if profile.Status != "active" || !profile.Verified {
		return "", http.StatusForbidden
	}
	return profile.Account, http.StatusOK
}

func (s *issuer) link(w http.ResponseWriter, r *http.Request) {
	var input struct {
		DeviceKey string `json:"device_key"`
	}
	if !readJSON(w, r, &input) {
		return
	}
	device, err := hex.DecodeString(input.DeviceKey)
	if err != nil || len(device) != 32 {
		deny(w, http.StatusBadRequest)
		return
	}
	owner, code := s.verifiedAccount(r)
	if code != http.StatusOK {
		deny(w, code)
		return
	}
	deviceHash := sha256.Sum256(append([]byte(namespace+":"), device...))
	subject := hex.EncodeToString(deviceHash[:])
	pid := binary.BigEndian.Uint64(deviceHash[:8]) & ((1 << 52) - 1)
	if pid == 0 {
		deny(w, http.StatusBadRequest)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
	defer cancel()
	ctx = s.core(ctx)
	link, err := s.links.ReserveLink(ctx, &accountv1.ReserveLinkRequest{AccountId: owner, Namespace: namespace,
		SubjectId: subject, IdempotencyKey: "reserve:" + subject})
	if err != nil {
		if status.Code(err) == codes.AlreadyExists || status.Code(err) == codes.FailedPrecondition {
			deny(w, http.StatusConflict)
		} else {
			deny(w, http.StatusServiceUnavailable)
		}
		return
	}
	activated, err := s.links.ActivateLink(ctx, &accountv1.ActivateLinkRequest{
		LinkId: link.GetLinkId(), IdempotencyKey: "activate:" + subject})
	if err != nil || activated.GetState() != accountv1.LinkState_LINK_STATE_ACTIVE {
		deny(w, http.StatusServiceUnavailable)
		return
	}
	value := make([]byte, 32)
	if _, err := rand.Read(value); err != nil {
		deny(w, http.StatusServiceUnavailable)
		return
	}
	now := time.Now()
	s.mu.Lock()
	for key, item := range s.tickets {
		if !now.Before(item.expires) {
			delete(s.tickets, key)
			continue
		}
		if item.pid == pid && item.subject != subject {
			s.mu.Unlock()
			deny(w, http.StatusConflict)
			return
		}
	}
	if len(s.tickets) >= 256 {
		s.mu.Unlock()
		deny(w, http.StatusTooManyRequests)
		return
	}
	s.tickets[sha256.Sum256(value)] = ticket{owner, subject, pid, now.Add(ticketLifetime)}
	s.mu.Unlock()
	jsonResponse(w, http.StatusCreated, map[string]any{"game_token": hex.EncodeToString(value), "pid": pid, "expires_in": int(ticketLifetime / time.Second)})
}

func (s *issuer) identity(w http.ResponseWriter, r *http.Request) {
	if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Internal-Key")), []byte(s.gameKey)) != 1 {
		deny(w, http.StatusUnauthorized)
		return
	}
	var input struct {
		Token string `json:"id_token"`
		PID   uint64 `json:"pid"`
	}
	if !readJSON(w, r, &input) {
		return
	}
	var selected ticket
	s.mu.Lock()
	if input.Token != "" && input.PID == 0 {
		value, err := hex.DecodeString(input.Token)
		if err == nil && len(value) == 32 {
			selected = s.tickets[sha256.Sum256(value)]
		}
	} else if input.Token == "" && input.PID != 0 {
		for _, item := range s.tickets {
			if item.pid == input.PID && item.expires.After(selected.expires) {
				selected = item
			}
		}
	}
	s.mu.Unlock()
	if selected.account == "" || !time.Now().Before(selected.expires) {
		if input.Token == "" && input.PID != 0 {
			deny(w, http.StatusNotFound)
		} else {
			deny(w, http.StatusUnauthorized)
		}
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
	defer cancel()
	ctx = s.core(ctx)
	link, err := s.links.GetLinkBySubject(ctx, &accountv1.GetLinkBySubjectRequest{Namespace: namespace, SubjectId: selected.subject})
	if status.Code(err) == codes.NotFound || (err == nil && (link.GetAccountId() != selected.account || link.GetState() != accountv1.LinkState_LINK_STATE_ACTIVE)) {
		deny(w, http.StatusForbidden)
		return
	}
	if err != nil {
		deny(w, http.StatusServiceUnavailable)
		return
	}
	account, err := s.users.GetAccount(ctx, &accountv1.GetAccountRequest{AccountId: selected.account})
	if status.Code(err) == codes.NotFound || (err == nil && (account.GetStatus() != accountv1.AccountStatus_ACCOUNT_STATUS_ACTIVE || !account.GetEmailVerified())) {
		deny(w, http.StatusForbidden)
		return
	}
	if err != nil {
		deny(w, http.StatusServiceUnavailable)
		return
	}
	jsonResponse(w, http.StatusOK, gameIdentity{selected.pid, fmt.Sprintf("%016x", selected.pid), account.GetDisplayName(),
		account.GetCountry(), selected.account, []string{}})
}

func main() {
	coreHTTP := os.Getenv("RC_CORE_HTTP")
	coreGRPC := os.Getenv("RC_CORE_GRPC")
	coreKey := os.Getenv("RC_CORE_KEY")
	gameKey := os.Getenv("RC_IDENTITY_KEY")
	addr := os.Getenv("RC_IDENTITY_LISTEN")
	if coreHTTP == "" || coreGRPC == "" || addr == "" || len(coreKey) < 32 || len(gameKey) < 32 || coreKey == gameKey {
		log.Fatal("account-lab identity configuration is missing or unsafe")
	}
	conn, err := grpc.NewClient(coreGRPC, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Fatal(err)
	}
	defer conn.Close()
	service := &issuer{coreURL: coreHTTP, coreKey: coreKey, gameKey: gameKey,
		client: &http.Client{Timeout: 4 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},
		links:  accountv1.NewLinksClient(conn), users: accountv1.NewIdentityClient(conn), tickets: make(map[[32]byte]ticket)}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusOK, map[string]string{"status": "ok"})
	})
	mux.HandleFunc("POST /v1/devices/link", service.link)
	mux.HandleFunc("POST /internal/switch/identity", service.identity)
	server := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 8 * time.Second,
		WriteTimeout: 10 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 8192}
	log.Fatal(server.ListenAndServe())
}
