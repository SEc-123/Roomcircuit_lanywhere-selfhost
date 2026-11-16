package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"image"
	"image/jpeg"
	"io"
	"net/http"
	"net/url"
	"os"
	"sort"
	"time"

	"roomcircuit.local/nx-adapter/wire"
)

const nativeAvatarMax = 192 << 10

var errInactiveProvisionalProfile = errors.New("provisional profile no longer active")

type nativeBAAS struct {
	adapter  *adapter
	profiles *nativeProfileStore
	cdn      string
}

func (a *adapter) addNativeBAASRoutes(mux *http.ServeMux) error {
	profiles, err := newNativeProfileStore(a.store)
	if err != nil {
		return err
	}
	cdn := os.Getenv("NX_LAB_BAAS_CDN_ORIGIN")
	if cdn == "" {
		cdn = a.issuer
	}
	if _, valid := origin(cdn, "https"); !valid {
		return errors.New("native avatar service requires an owned HTTPS origin")
	}
	b := &nativeBAAS{adapter: a, profiles: profiles, cdn: cdn}
	a.baas = b
	mux.HandleFunc("GET /1.0.0/users/{id}", b.user)
	mux.HandleFunc("PATCH /1.0.0/users/{id}", b.patch)
	mux.HandleFunc("POST /1.0.0/image_upload", b.uploadAvatar)
	mux.HandleFunc("GET /1/{id}", b.avatar)
	mux.HandleFunc("GET /2/{id}", b.avatar)
	mux.HandleFunc("GET /v1/users/{id}/membership", a.nativeMembership)
	mux.HandleFunc("GET /v1/users/{id}/memberships", a.memberships)
	return a.addNativeAAUTH(mux)
}

func (b *nativeBAAS) authorized(w http.ResponseWriter, r *http.Request, id string) (player, bool) {
	who, status := b.adapter.profilePrincipal(r.Context(), bearer(r))
	if status != 200 {
		b.adapter.baasFailure(w, r, status, "invalid_token")
		return player{}, false
	}
	if !validNativeUserID(who.User) || (who.Account == "") != (who.PID == 0) || (id != "" && who.User != id) {
		b.adapter.baasFailure(w, r, 403, "insufficient_scope")
		return player{}, false
	}
	return who, true
}

func baasBindingID(who player) string {
	subject := "roomcircuit-baas-binding-v1\x00" + who.Account
	if provisionalProfile(who) {
		subject = "roomcircuit-baas-provisional-binding-v1\x00" + who.User
	}
	sum := sha256.Sum256([]byte(subject))
	return hex.EncodeToString(sum[:8])
}

func provisionalProfile(who player) bool {
	return who.Account == "" && who.PID == 0
}

func (b *nativeBAAS) snapshot(who player, record nativeProfile) wire.BaaSUserInput {
	draft := provisionalProfile(who)
	b.adapter.mu.Lock()
	var deviceIDs []string
	var firstCreated, firstLinked int64
	for id, entry := range b.adapter.devices {
		if (draft && entry.provisional == who.User) ||
			(!draft && entry.owner.User == who.User && entry.owner.Account == who.Account && entry.provisional == "") {
			deviceIDs = append(deviceIDs, id)
			if entry.createdAt > 0 && (firstCreated == 0 || entry.createdAt < firstCreated) {
				firstCreated = entry.createdAt
			}
			if entry.linkedAt > 0 && (firstLinked == 0 || entry.linkedAt < firstLinked) {
				firstLinked = entry.linkedAt
			}
		}
	}
	b.adapter.mu.Unlock()
	if record.CreatedAt == 0 {
		record.CreatedAt = firstCreated
	}
	record.UpdatedAt = max(record.CreatedAt, record.UpdatedAt)
	if record.Profile.Nickname == "" {
		record.Profile.Nickname = who.Nickname
		record.Profile.NicknameUpdatedAt = record.CreatedAt
	}
	sort.Strings(deviceIDs)
	country := who.Country
	var account *wire.AccountLink
	if draft {
		country = record.Country
	} else {
		account = &wire.AccountLink{ID: naSubject(who), CreatedAt: firstLinked, UpdatedAt: firstLinked}
	}
	return wire.BaaSUserInput{
		ID: who.User, DeviceBindingID: baasBindingID(who), CDNOrigin: b.cdn, Country: country,
		UsernameOnly: who.AccountType == "username",
		Birthday:     record.Birthday, CreatedAt: record.CreatedAt, UpdatedAt: record.UpdatedAt,
		BlocksUpdatedAt: record.CreatedAt, FriendsUpdatedAt: record.CreatedAt,
		Profile: record.Profile, DeviceIDs: deviceIDs,
		Account: account,
	}
}

func (b *nativeBAAS) withProfileAuthority(who player, operation func() error) error {
	if !provisionalProfile(who) {
		return operation()
	}
	// Serialize a draft operation with the binding transition. Once the
	// device slot is linked, a previously authenticated draft cannot write
	// the same record or read a later version of the linked profile.
	b.adapter.mu.Lock()
	defer b.adapter.mu.Unlock()
	now := time.Now()
	for _, entry := range b.adapter.devices {
		if entry.provisional == who.User && !entry.registrationExpired(now) {
			return operation()
		}
	}
	return errInactiveProvisionalProfile
}

func (b *nativeBAAS) readOwnedProfile(who player) (nativeProfile, error) {
	var record nativeProfile
	err := b.withProfileAuthority(who, func() error {
		var err error
		record, err = b.profiles.read(who.User)
		return err
	})
	return record, err
}

func (b *nativeBAAS) writeOwnedProfile(who player, record nativeProfile) error {
	return b.withProfileAuthority(who, func() error { return b.profiles.write(who.User, record) })
}

func (b *nativeBAAS) profileStorageFailure(w http.ResponseWriter, r *http.Request, err error) {
	if errors.Is(err, errInactiveProvisionalProfile) {
		b.adapter.baasFailure(w, r, 401, "invalid_token")
		return
	}
	b.adapter.baasFailure(w, r, 503, "under_maintenance")
}

func (b *nativeBAAS) user(w http.ResponseWriter, r *http.Request) {
	who, ok := b.authorized(w, r, r.PathValue("id"))
	if !ok {
		return
	}
	record, err := b.readOwnedProfile(who)
	if err != nil {
		b.profileStorageFailure(w, r, err)
		return
	}
	result, err := wire.BuildBaaSUser(b.snapshot(who, record))
	if err != nil {
		b.adapter.baasFailure(w, r, 409, "complete_account_profile")
		return
	}
	respond(w, 200, result)
}

func (b *nativeBAAS) patch(w http.ResponseWriter, r *http.Request) {
	who, ok := b.authorized(w, r, r.PathValue("id"))
	if !ok {
		return
	}
	var ops []wire.ProfilePatch
	body := json.NewDecoder(http.MaxBytesReader(w, r.Body, 32<<10))
	body.DisallowUnknownFields()
	if body.Decode(&ops) != nil || body.Decode(new(any)) != io.EOF || len(ops) == 0 || len(ops) > 8 {
		b.adapter.baasFailure(w, r, 400, "invalid_params")
		return
	}
	b.profiles.mu.Lock()
	defer b.profiles.mu.Unlock()
	record, err := b.readOwnedProfile(who)
	if err != nil {
		b.profileStorageFailure(w, r, err)
		return
	}
	input := b.snapshot(who, record)
	current, err := wire.BuildBaaSUser(input)
	if err != nil {
		b.adapter.baasFailure(w, r, 409, "complete_account_profile")
		return
	}
	var edits []wire.ProfilePatch
	metadataChanged := false
	seen := make(map[string]bool, len(ops))
	for _, op := range ops {
		if op.Op != "replace" || seen[op.Path] {
			b.adapter.baasFailure(w, r, 400, "invalid_params")
			return
		}
		seen[op.Path] = true
		switch op.Path {
		case "/thumbnailUrl":
			// An avatar upload already chose the owned URL. Allow the console
			// to confirm it without persisting an arbitrary external URL.
			candidate, err := url.Parse(op.Value)
			expected, expectedErr := url.Parse(current.ThumbnailURL)
			if err != nil || expectedErr != nil || candidate.User != nil || candidate.Fragment != "" ||
				candidate.Scheme != expected.Scheme || candidate.Host != expected.Host || candidate.Path != expected.Path ||
				(candidate.RawQuery != "" && candidate.RawQuery != expected.RawQuery) {
				b.adapter.baasFailure(w, r, 400, "invalid_params")
				return
			}
		case "/country", "/birthday":
			if provisionalProfile(who) {
				oldCountry, oldBirthday := record.Country, record.Birthday
				if !applyProvisionalIdentity(&record, op) {
					b.adapter.baasFailure(w, r, 400, "invalid_params")
					return
				}
				metadataChanged = metadataChanged || record.Country != oldCountry || record.Birthday != oldBirthday
				continue
			}
			if (op.Path == "/country" && op.Value != current.Country) || (op.Path == "/birthday" && op.Value != current.Birthday) {
				b.adapter.baasFailure(w, r, 409, "update_account_profile")
				return
			}
		default:
			edits = append(edits, op)
		}
	}
	if len(edits) == 0 && !metadataChanged {
		respond(w, 200, current)
		return
	}
	now := max(time.Now().Unix(), record.UpdatedAt)
	if len(edits) > 0 {
		profile, err := wire.ApplyBaaSProfilePatch(who.User, r.PathValue("id"), input.Profile, edits, now)
		if err != nil {
			b.adapter.baasFailure(w, r, 400, "invalid_params")
			return
		}
		record.Profile = profile
	}
	record.CreatedAt, record.UpdatedAt = input.CreatedAt, now
	result, err := wire.BuildBaaSUser(b.snapshot(who, record))
	if err != nil {
		b.adapter.baasFailure(w, r, 409, "complete_account_profile")
		return
	}
	if err := b.writeOwnedProfile(who, record); err != nil {
		b.profileStorageFailure(w, r, err)
		return
	}
	respond(w, 200, result)
}

func applyProvisionalIdentity(record *nativeProfile, op wire.ProfilePatch) bool {
	switch op.Path {
	case "/country":
		if op.Value != "" && (len(op.Value) != 2 || op.Value[0] < 'A' || op.Value[0] > 'Z' ||
			op.Value[1] < 'A' || op.Value[1] > 'Z') {
			return false
		}
		record.Country = op.Value
	case "/birthday":
		if op.Value == "" || op.Value == "0000-00-00" {
			record.Birthday = ""
			return true
		}
		date, err := time.Parse(time.DateOnly, op.Value)
		if err != nil || date.Year() < 1 || date.Format(time.DateOnly) != op.Value || date.After(time.Now().UTC()) {
			return false
		}
		record.Birthday = op.Value
	default:
		return false
	}
	return true
}

func (b *nativeBAAS) uploadAvatar(w http.ResponseWriter, r *http.Request) {
	who, ok := b.authorized(w, r, "")
	if !ok {
		return
	}
	var request struct {
		RawContent     string `json:"rawContent"`
		OwnerID        string `json:"ownerId"`
		AllowTransform bool   `json:"allowTransform"`
	}
	body := json.NewDecoder(http.MaxBytesReader(w, r.Body, (256<<10)+1024))
	body.DisallowUnknownFields()
	if body.Decode(&request) != nil || body.Decode(new(any)) != io.EOF || len(request.RawContent) > base64.StdEncoding.EncodedLen(nativeAvatarMax) {
		b.adapter.baasFailure(w, r, 400, "invalid_raw_content")
		return
	}
	if request.OwnerID != "" && request.OwnerID != who.User {
		b.adapter.baasFailure(w, r, 403, "insufficient_scope")
		return
	}
	raw, err := base64.StdEncoding.Strict().DecodeString(request.RawContent)
	if err != nil {
		b.adapter.baasFailure(w, r, 400, "invalid_raw_content")
		return
	}
	config, err := jpeg.DecodeConfig(bytes.NewReader(raw))
	if err != nil || config.Width < 1 || config.Height < 1 || config.Width > 512 || config.Height > 512 {
		b.adapter.baasFailure(w, r, 400, "invalid_raw_content")
		return
	}
	decoded, err := jpeg.Decode(bytes.NewReader(raw))
	if err != nil || decoded.Bounds().Dx() != config.Width || decoded.Bounds().Dy() != config.Height {
		b.adapter.baasFailure(w, r, 400, "invalid_raw_content")
		return
	}
	var clean bytes.Buffer
	if err := jpeg.Encode(&clean, decoded, &jpeg.Options{Quality: 82}); err != nil || clean.Len() > nativeAvatarMax {
		b.adapter.baasFailure(w, r, 400, "invalid_raw_content")
		return
	}
	b.profiles.mu.Lock()
	defer b.profiles.mu.Unlock()
	record, err := b.readOwnedProfile(who)
	if err != nil {
		b.profileStorageFailure(w, r, err)
		return
	}
	now := max(time.Now().Unix(), record.UpdatedAt)
	if now <= record.Profile.AvatarUploadedAt {
		now = record.Profile.AvatarUploadedAt + 1
	}
	result, err := wire.BuildAvatarUpload(who.User, who.User, b.cdn, now)
	if err != nil {
		b.adapter.baasFailure(w, r, 503, "under_maintenance")
		return
	}
	if record.CreatedAt == 0 {
		record.CreatedAt = b.snapshot(who, record).CreatedAt
	}
	record.Avatar, record.Profile.AvatarUploadedAt, record.UpdatedAt = clean.Bytes(), now, now
	if err := b.writeOwnedProfile(who, record); err != nil {
		b.profileStorageFailure(w, r, err)
		return
	}
	respond(w, 200, result)
}

func (b *nativeBAAS) avatar(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if id == "default" {
		w.Header().Set("Content-Type", "image/jpeg")
		w.Header().Set("Cache-Control", "public, max-age=86400")
		placeholder := image.NewGray(image.Rect(0, 0, 16, 16))
		for i := range placeholder.Pix {
			placeholder.Pix[i] = 220
		}
		_ = jpeg.Encode(w, placeholder, nil)
		return
	}
	who, ok := b.authorized(w, r, id)
	if !ok {
		return
	}
	record, err := b.readOwnedProfile(who)
	if err != nil {
		b.profileStorageFailure(w, r, err)
		return
	}
	if len(record.Avatar) == 0 {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Cache-Control", "private, no-store")
	_, _ = w.Write(record.Avatar)
}

func (a *adapter) membershipPlayer(w http.ResponseWriter, r *http.Request, allowLegacyID bool) (player, bool) {
	who, status := a.authenticateNAAccess(r.Context(), bearer(r))
	viaNA := status == 200
	if status == 401 {
		who, status = a.authenticated(r.Context(), bearer(r), "token")
	}
	if status != 200 {
		deny(w, status, "membership_unauthorized")
		return player{}, false
	}
	id := r.PathValue("id")
	if id != naSubject(who) && !(allowLegacyID && !viaNA && id == who.User) {
		deny(w, 403, "membership_unauthorized")
		return player{}, false
	}
	if viaNA {
		a.mu.Lock()
		linked := false
		for _, device := range a.devices {
			if device.owner.Account == who.Account && device.owner.PID == who.PID &&
				who.GrantID != "" && device.grant == durableGrantPrefix+who.GrantID && device.provisional == "" {
				linked = true
				break
			}
		}
		a.mu.Unlock()
		if !linked {
			deny(w, 403, "membership_unauthorized")
			return player{}, false
		}
	}
	return who, true
}

func (a *adapter) nativeMembership(w http.ResponseWriter, r *http.Request) {
	who, ok := a.membershipPlayer(w, r, true)
	if !ok {
		return
	}
	active := who.Application == a.application && r.URL.Query().Get("application_id") == a.application
	var expires int64
	if active {
		expires = time.Now().Add(loginTTL).Unix()
	}
	respond(w, 200, map[string]any{"membership": map[string]any{"active": active, "expires_at": expires}})
}

func (a *adapter) memberships(w http.ResponseWriter, r *http.Request) {
	who, ok := a.membershipPlayer(w, r, false)
	if !ok {
		return
	}
	active := who.Application == a.application
	var expires int64
	if active {
		expires = time.Now().Add(loginTTL).Unix()
	}
	items := []map[string]any{
		{"context": "default", "user_id": r.PathValue("id"), "membership": map[string]any{"active": active, "expires_at": expires}},
		{"context": "ex", "user_id": r.PathValue("id"), "membership": map[string]any{"active": false, "expires_at": 0}},
	}
	respond(w, 200, map[string]any{"count": len(items), "items": items, "total": len(items)})
}
