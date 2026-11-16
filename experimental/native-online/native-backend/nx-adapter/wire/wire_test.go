// SPDX-License-Identifier: AGPL-3.0-only
package wire

import (
	"bytes"
	"encoding/json"
	"errors"
	"net/url"
	"os"
	"reflect"
	"strings"
	"testing"
)

const (
	userID   = "123456789abcdef0"
	naID     = "219876543abcdef0"
	deviceID = "abcdef0123456789"
	otherID  = "0123456789abcdef"
	cdn      = "https://cdn.roomcircuit.example.test/media"
)

func testUser() BaaSUserInput {
	return BaaSUserInput{
		ID: userID, DeviceBindingID: "room-binding-10", CDNOrigin: cdn, Country: "IE", Birthday: "2001-04-07",
		CreatedAt: 100, UpdatedAt: 130, BlocksUpdatedAt: 100, FriendsUpdatedAt: 103,
		Profile:   BaaSProfile{Nickname: "Joueur", AccountBlob: "01606150AAAA", NicknameUpdatedAt: 125, AvatarUploadedAt: 130},
		DeviceIDs: []string{deviceID}, Account: &AccountLink{ID: naID, CreatedAt: 102, UpdatedAt: 105},
		FriendCode: &FriendCodeLink{ID: "SW-1234-5678-9012", CreatedAt: 100, UpdatedAt: 103, RegenerableAt: 200},
	}
}

func TestBaaSLoginMatchesAcceptedShape(t *testing.T) {
	input := testUser()
	login, err := BuildBaaSLogin(input, BaaSLoginCredentials{IDToken: "own-id", AccessToken: "own-access", ExpiresIn: 300})
	if err != nil {
		t.Fatal(err)
	}
	shapeFixture(t, "accepted-login-shape.json", login)
	if login.User.ETag != `"room-binding-10"` || login.User.Links.Account.ID != naID ||
		login.User.Links.FriendCode.ID != "1234-5678-9012" || login.User.Extras.Self.IsMediaUploaded == nil ||
		!*login.User.Extras.Self.IsMediaUploaded || login.TokenType != "Bearer" {
		t.Fatal("login did not retain the supplied device identity and owned profile")
	}
	input.Account.ID = otherID
	if login.User.Links.Account.ID != naID {
		t.Fatal("the login result aliases the caller's profile storage")
	}
}

func TestBaaSRegistrationScopesPassword(t *testing.T) {
	input := testUser()
	input.DeviceIDs = append(input.DeviceIDs, otherID)
	password := strings.Repeat("c", 40)
	registered, err := BuildBaaSRegistration(input, deviceID, password)
	if err != nil || registered.DeviceAccounts[0].Password != password || registered.DeviceAccounts[1].Password != "" {
		t.Fatalf("registration did not restrict its credential to the selected device: %v", err)
	}
	readback, err := BuildBaaSUser(input)
	if err != nil || readback.DeviceAccounts[0].Password != "" || readback.DeviceAccounts[1].Password != "" {
		t.Fatalf("ordinary account view exposed a registration credential: %v", err)
	}
	if _, err := BuildBaaSRegistration(input, naID, password); !errors.Is(err, ErrOwnerMismatch) {
		t.Fatal("a device outside the projection received a registration response")
	}
	if _, err := BuildBaaSRegistration(input, deviceID, "malformed"); !errors.Is(err, ErrInvalidField) {
		t.Fatal("registration accepted a malformed device password")
	}
}

func TestBaaSAvatarAndPatchReadback(t *testing.T) {
	input := testUser()
	input.Profile.AvatarUploadedAt = 0
	withoutAvatar, err := BuildBaaSUser(input)
	if err != nil || withoutAvatar.ThumbnailURL != cdn+"/1/default" || withoutAvatar.Extras.Self.IsMediaUploaded == nil ||
		*withoutAvatar.Extras.Self.IsMediaUploaded {
		t.Fatalf("a profile blob advertised an image that the caller had not stored: %v", err)
	}
	const uploadedAt int64 = 150
	upload, err := BuildAvatarUpload(userID, userID, cdn, uploadedAt)
	if err != nil || upload.Content.Format != "jpg" {
		t.Fatalf("unexpected avatar response: %v", err)
	}
	input.Profile.AvatarUploadedAt = uploadedAt
	input.UpdatedAt = 155
	patch := []ProfilePatch{{"replace", "/nickname", "遊ぶ人"}, {"replace", "/extras/self/nxAccount", "QmFzZVByb2ZpbGU="}}
	updated, err := ApplyBaaSProfilePatch(userID, userID, input.Profile, patch, input.UpdatedAt)
	if err != nil || updated.Nickname != patch[0].Value || updated.AccountBlob != patch[1].Value || updated.NicknameUpdatedAt != 155 {
		t.Fatalf("owner profile PATCH was not preserved exactly: %v", err)
	}
	input.Profile = updated
	view, err := BuildBaaSUser(input)
	if err != nil || view.ThumbnailURL != upload.Content.URL || view.Thumbnail2URL != cdn+"/2/"+userID+"?v=150" ||
		view.ThumbnailUploadedAt != uploadedAt || view.Nickname != updated.Nickname || view.Extras.Self.AccountBlob != updated.AccountBlob {
		t.Fatalf("avatar and profile PATCH did not persist the same readback: %v", err)
	}
	if again, err := BuildBaaSUser(input); err != nil || !reflect.DeepEqual(view, again) {
		t.Fatal("unchanged metadata produced a different readback")
	}
	if _, err := BuildAvatarUpload(otherID, userID, cdn, uploadedAt); !errors.Is(err, ErrOwnerMismatch) {
		t.Fatal("avatar response was rendered for another owner")
	}
	if got, err := ApplyBaaSProfilePatch(otherID, userID, updated, patch, 156); got != updated || !errors.Is(err, ErrOwnerMismatch) {
		t.Fatal("another owner's PATCH did not preserve the existing state")
	}
	invalidPatch := []ProfilePatch{patch[0], {"replace", "/extras/self/nxAccount", "with\x00invalid-control"}}
	if got, err := ApplyBaaSProfilePatch(userID, userID, updated, invalidPatch, 156); got != updated || !errors.Is(err, ErrInvalidField) {
		t.Fatal("a PATCH with an invalid trailing operation was partially applied")
	}
}

func TestMalformedWireFields(t *testing.T) {
	for _, test := range []struct {
		name   string
		mutate func(*BaaSUserInput)
	}{
		{"path injection in ID", func(input *BaaSUserInput) { input.ID = "../../other/user" }},
		{"header injection in etag", func(input *BaaSUserInput) { input.DeviceBindingID = "x\r\ny" }},
		{"duplicate device", func(input *BaaSUserInput) { input.DeviceIDs = []string{deviceID, deviceID} }},
		{"bad media base", func(input *BaaSUserInput) { input.CDNOrigin = "https://user:password@example.test/" }},
		{"oversized media URL", func(input *BaaSUserInput) { input.CDNOrigin = cdn + "/" + strings.Repeat("x", 250) }},
		{"unreal birthday", func(input *BaaSUserInput) { input.Birthday = "2026-02-29" }},
		{"invalid UTF8 nickname", func(input *BaaSUserInput) { input.Profile.Nickname = "\xff" }},
		{"unsupported presence", func(input *BaaSUserInput) { input.Permissions.Presence = "EVERYONE" }},
	} {
		t.Run(test.name, func(t *testing.T) {
			input := testUser()
			test.mutate(&input)
			if value, err := BuildBaaSUser(input); err == nil || value.ID != "" {
				t.Fatal("malformed profile produced a partial response")
			}
		})
	}
	if _, err := BuildBaaSLogin(testUser(), BaaSLoginCredentials{IDToken: "same", AccessToken: "same", ExpiresIn: 300}); !errors.Is(err, ErrInvalidField) {
		t.Fatal("a single credential was passed off as distinct token types")
	}
}

func TestNAProfileShapeUsesVerifiedOwnValues(t *testing.T) {
	input := NAProfileInput{ID: naID, AccessSubject: naID, Nickname: "遊ぶ人", Email: "player@example.test", Country: "IE",
		Birthday: "2001-04-07", EmailVerified: true, Language: "en-IE", CreatedAt: 100, UpdatedAt: 110,
		IconURI: cdn + "/1/" + userID, Timezone: NATimezone{ID: "Etc/UTC", Name: "UTC", UTCOffset: "+00:00"}}
	profile, err := BuildNAProfile(input)
	if err != nil {
		t.Fatal(err)
	}
	shapeFixture(t, "na-profile-shape.json", profile)
	if profile.ID != input.AccessSubject || profile.Nickname != input.Nickname || profile.ScreenName != "pl•••@e••••" ||
		profile.Birthday != input.Birthday || profile.Country != input.Country || profile.Summary.Nintendo.HasMembership ||
		profile.AgreedTerms.EULA != nil {
		t.Fatal("profile changed supplied data or invented a grant or term agreement")
	}
	input.AccessSubject = otherID
	if _, err := BuildNAProfile(input); !errors.Is(err, ErrOwnerMismatch) {
		t.Fatal("NA UserInfo exposed another account")
	}
	input.AccessSubject, input.EmailVerified = naID, false
	if _, err := BuildNAProfile(input); !errors.Is(err, ErrInvalidField) {
		t.Fatal("NA UserInfo served an unverified account")
	}
	input.EmailVerified, input.Timezone.UTCOffset = true, "+08:00"
	if _, err := BuildNAProfile(input); !errors.Is(err, ErrInvalidField) {
		t.Fatal("NA UserInfo presented conflicting timezone formats")
	}
}

func TestBaaSErrorScopeAndShape(t *testing.T) {
	const issuer = "https://accounts.roomcircuit.example.test"
	value, err := BuildBaaSError(issuer, 403, "wrong_device_owner", "Forbidden", "Permission denied", "/1.0.0/users/"+userID)
	if err != nil {
		t.Fatal(err)
	}
	shape(t, "error", decodeValue(t, value), map[string]any{"status": "int", "errorCode": "str", "title": "str", "detail": "str", "instance": "str", "type": "str"})
	uri, err := url.Parse(value.Type)
	if err != nil || uri.Host != "accounts.roomcircuit.example.test" || uri.Path != "/errors/1.0.0/403/wrong_device_owner" {
		t.Fatal("error response used an unrelated problem origin")
	}
	if _, err := BuildBaaSError(issuer, 403, "invalid_token", "Forbidden", "", "/route?access_token=private"); !errors.Is(err, ErrInvalidField) {
		t.Fatal("error envelope accepted a request query with private data")
	}
}

func shapeFixture(t *testing.T, fixture string, value any) {
	t.Helper()
	wantBytes, err := os.ReadFile("testdata/" + fixture)
	if err != nil {
		t.Fatal(err)
	}
	var want any
	if err = json.Unmarshal(wantBytes, &want); err != nil {
		t.Fatal(err)
	}
	shape(t, "root", decodeValue(t, value), want)
}

func decodeValue(t *testing.T, value any) any {
	t.Helper()
	body, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	decoder := json.NewDecoder(bytes.NewReader(body))
	decoder.UseNumber()
	var decoded any
	if err = decoder.Decode(&decoded); err != nil {
		t.Fatal(err)
	}
	return decoded
}

func shape(t *testing.T, path string, actual, expected any) {
	t.Helper()
	switch want := expected.(type) {
	case map[string]any:
		object, ok := actual.(map[string]any)
		if !ok || len(want) != len(object) {
			t.Fatalf("%s: unexpected object shape", path)
		}
		for key, member := range want {
			value, ok := object[key]
			if !ok {
				t.Fatalf("%s.%s: field absent", path, key)
			}
			shape(t, path+"."+key, value, member)
		}
	case []any:
		list, ok := actual.([]any)
		if !ok || len(list) != len(want) {
			t.Fatalf("%s: unexpected list shape", path)
		}
		for i, member := range want {
			shape(t, path+"[]", list[i], member)
		}
	case string:
		if !typeMatches(actual, want) {
			t.Fatalf("%s: expected %s, got %T", path, want, actual)
		}
	case nil:
		if actual != nil {
			t.Fatalf("%s: expected null, got %T", path, actual)
		}
	default:
		t.Fatalf("%s: unknown shape marker", path)
	}
}

func typeMatches(value any, marker string) bool {
	switch marker {
	case "str":
		_, valid := value.(string)
		return valid
	case "bool":
		_, valid := value.(bool)
		return valid
	case "int":
		number, valid := value.(json.Number)
		_, err := number.Int64()
		return valid && err == nil
	case "object", "<object>":
		_, valid := value.(map[string]any)
		return valid
	default:
		return false
	}
}
