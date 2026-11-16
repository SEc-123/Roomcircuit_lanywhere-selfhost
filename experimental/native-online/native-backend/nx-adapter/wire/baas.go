// SPDX-License-Identifier: AGPL-3.0-only
// Field layout adapted from OpenPak nx-baas cf93fd71, internal/friends/objects.go
// and internal/baas/baas.go. RoomCircuit supplies its own verified data.
package wire

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

const maxProfileBlobBytes = 8192

type BaaSProfile struct {
	Nickname, AccountBlob               string
	NicknameUpdatedAt, AvatarUploadedAt int64
}

type AccountLink struct {
	ID        string `json:"id"`
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

type FriendCodeLink struct {
	ID            string `json:"id"`
	CreatedAt     int64  `json:"createdAt"`
	UpdatedAt     int64  `json:"updatedAt"`
	Regenerable   bool   `json:"regenerable"`
	RegenerableAt int64  `json:"regenerableAt"`
}

type BaaSPermissions struct {
	Friends                       string `json:"friends"`
	Presence                      string `json:"presence"`
	PresenceUpdatedAt             int64  `json:"presenceUpdatedAt"`
	FriendRequestReception        bool   `json:"friendRequestReception"`
	PersonalAnalytics             bool   `json:"personalAnalytics"`
	PersonalAnalyticsUpdatedAt    int64  `json:"personalAnalyticsUpdatedAt"`
	PersonalNotification          bool   `json:"personalNotification"`
	PersonalNotificationUpdatedAt int64  `json:"personalNotificationUpdatedAt"`
}

type BaaSUserInput struct {
	ID, DeviceBindingID, CDNOrigin, Country, Birthday       string
	CreatedAt, UpdatedAt, BlocksUpdatedAt, FriendsUpdatedAt int64
	Profile                                                 BaaSProfile
	DeviceIDs                                               []string
	Account                                                 *AccountLink
	FriendCode                                              *FriendCodeLink
	Permissions                                             BaaSPermissions
}

type BaaSDeviceAccount struct {
	ID       string `json:"id"`
	Password string `json:"password,omitempty"`
}

type BaaSLinks struct {
	Account    *AccountLink    `json:"nintendoAccount,omitempty"`
	FriendCode *FriendCodeLink `json:"friendCode,omitempty"`
}

type BaaSPlayLog struct {
	PlayLog string `json:"playLog"`
}

type BaaSFriendsExtras struct {
	BaaSPlayLog
	IsChatActivated bool `json:"isChatActivated"`
	IsChatInvitable bool `json:"isChatInvitable"`
}

type BaaSSelfExtras struct {
	BaaSPlayLog
	IsMediaUploaded *bool  `json:"isMediaUploaded,omitempty"`
	AccountBlob     string `json:"nxAccount,omitempty"`
}

type BaaSExtras struct {
	Everyone         BaaSPlayLog       `json:"everyone"`
	FavoriteFriends  BaaSPlayLog       `json:"favoriteFriends"`
	FriendsOfFriends BaaSPlayLog       `json:"foaf"`
	Friends          BaaSFriendsExtras `json:"friends"`
	Self             BaaSSelfExtras    `json:"self"`
}

type BaaSUser struct {
	ID                  string              `json:"id"`
	Nickname            string              `json:"nickname"`
	Country             string              `json:"country"`
	Birthday            string              `json:"birthday"`
	CreatedAt           int64               `json:"createdAt"`
	UpdatedAt           int64               `json:"updatedAt"`
	NicknameUpdatedAt   int64               `json:"nicknameUpdatedAt"`
	BlocksUpdatedAt     int64               `json:"blocksUpdatedAt"`
	FriendsUpdatedAt    int64               `json:"friendsUpdatedAt"`
	Deleted             bool                `json:"deleted"`
	DeviceAccounts      []BaaSDeviceAccount `json:"deviceAccounts"`
	ETag                string              `json:"etag"`
	Extras              BaaSExtras          `json:"extras"`
	Links               BaaSLinks           `json:"links"`
	Permissions         BaaSPermissions     `json:"permissions"`
	ThumbnailURL        string              `json:"thumbnailUrl"`
	Thumbnail2URL       string              `json:"thumbnail2Url"`
	ThumbnailUploadedAt int64               `json:"thumbnailUploadedAt"`
}

type BaaSLoginCredentials struct {
	IDToken, AccessToken string
	ExpiresIn            int
}

type BaaSLogin struct {
	IDToken     string   `json:"idToken"`
	AccessToken string   `json:"accessToken"`
	TokenType   string   `json:"tokenType"`
	ExpiresIn   int      `json:"expiresIn"`
	User        BaaSUser `json:"user"`
}

func BuildBaaSUser(input BaaSUserInput) (BaaSUser, error) {
	if !validHexID(input.ID, 16) || !stableToken(input.DeviceBindingID) {
		return BaaSUser{}, invalid("BAAS user or binding ID")
	}
	linked := input.Account != nil
	if (linked || input.Country != "") && !validCountry(input.Country) {
		return BaaSUser{}, invalid("BAAS country")
	}
	if !validText(input.Profile.Nickname, 32, !linked) || !validText(input.Profile.AccountBlob, maxProfileBlobBytes, true) {
		return BaaSUser{}, invalid("BAAS profile")
	}
	if input.CreatedAt < 0 || input.UpdatedAt < input.CreatedAt || input.Profile.NicknameUpdatedAt < 0 ||
		input.Profile.AvatarUploadedAt < 0 || input.BlocksUpdatedAt < 0 || input.FriendsUpdatedAt < 0 {
		return BaaSUser{}, invalid("BAAS timestamps")
	}
	birthday := input.Birthday
	if birthday == "" {
		birthday = "0000-00-00" // Wire sentinel in the accepted fixture. It never enters the profile store.
	}
	if birthday != "0000-00-00" && !validDate(birthday) {
		return BaaSUser{}, invalid("BAAS birthday")
	}
	links, err := buildBaaSLinks(input.Account, input.FriendCode)
	if err != nil {
		return BaaSUser{}, err
	}
	devices := make([]BaaSDeviceAccount, 0, len(input.DeviceIDs))
	seen := make(map[string]bool, len(input.DeviceIDs))
	for _, id := range input.DeviceIDs {
		if !validDeviceID(id) || seen[id] {
			return BaaSUser{}, invalid("BAAS device ID")
		}
		seen[id] = true
		devices = append(devices, BaaSDeviceAccount{ID: id})
	}
	permissions := input.Permissions
	if permissions.Friends == "" {
		permissions.Friends = "SELF"
	}
	if permissions.Presence == "" {
		permissions.Presence = "SELF"
	}
	if !friendVisibility(permissions.Friends) || !presenceVisibility(permissions.Presence) || permissions.PresenceUpdatedAt < 0 ||
		permissions.PersonalAnalyticsUpdatedAt < 0 || permissions.PersonalNotificationUpdatedAt < 0 {
		return BaaSUser{}, invalid("BAAS permissions")
	}
	thumbnail, err := thumbnailURL(input.CDNOrigin, "1", input.ID, input.Profile.AvatarUploadedAt)
	if err != nil {
		return BaaSUser{}, err
	}
	thumbnail2, err := thumbnailURL(input.CDNOrigin, "2", input.ID, input.Profile.AvatarUploadedAt)
	if err != nil {
		return BaaSUser{}, err
	}
	self := BaaSSelfExtras{AccountBlob: input.Profile.AccountBlob}
	if input.Profile.AvatarUploadedAt > 0 || self.AccountBlob != "" {
		mediaUploaded := input.Profile.AvatarUploadedAt > 0
		self.IsMediaUploaded = &mediaUploaded
	}
	return BaaSUser{
		ID: input.ID, Nickname: input.Profile.Nickname, Country: input.Country, Birthday: birthday,
		CreatedAt: input.CreatedAt, UpdatedAt: input.UpdatedAt, NicknameUpdatedAt: input.Profile.NicknameUpdatedAt,
		BlocksUpdatedAt: input.BlocksUpdatedAt, FriendsUpdatedAt: input.FriendsUpdatedAt,
		DeviceAccounts: devices, ETag: strconv.Quote(input.DeviceBindingID), Extras: BaaSExtras{Self: self},
		Links: links, Permissions: permissions, ThumbnailURL: thumbnail, Thumbnail2URL: thumbnail2,
		ThumbnailUploadedAt: input.Profile.AvatarUploadedAt,
	}, nil
}

func friendVisibility(value string) bool {
	return value == "SELF" || value == "FRIENDS" || value == "EVERYONE"
}

func presenceVisibility(value string) bool {
	return value == "SELF" || value == "FRIENDS" || value == "FAVORITE_FRIENDS"
}

func BuildBaaSRegistration(input BaaSUserInput, deviceID, password string) (BaaSUser, error) {
	if !validDeviceID(deviceID) || !validHexID(password, 40) && !validHexID(password, 64) {
		return BaaSUser{}, invalid("BAAS registration credential")
	}
	user, err := BuildBaaSUser(input)
	if err != nil {
		return BaaSUser{}, err
	}
	for index, item := range user.DeviceAccounts {
		if item.ID == deviceID {
			user.DeviceAccounts[index].Password = password
			return user, nil
		}
	}
	return BaaSUser{}, ErrOwnerMismatch
}

func BuildBaaSLogin(input BaaSUserInput, credentials BaaSLoginCredentials) (BaaSLogin, error) {
	if credentials.ExpiresIn <= 0 || !validText(credentials.IDToken, 4096, false) ||
		!validText(credentials.AccessToken, 4096, false) || credentials.IDToken == credentials.AccessToken {
		return BaaSLogin{}, invalid("BAAS login credential")
	}
	user, err := BuildBaaSUser(input)
	if err != nil {
		return BaaSLogin{}, err
	}
	return BaaSLogin{IDToken: credentials.IDToken, AccessToken: credentials.AccessToken, TokenType: "Bearer",
		ExpiresIn: credentials.ExpiresIn, User: user}, nil
}

func buildBaaSLinks(account *AccountLink, friendCode *FriendCodeLink) (BaaSLinks, error) {
	var links BaaSLinks
	if account != nil {
		if !validHexID(account.ID, 16) || account.CreatedAt < 0 || account.UpdatedAt < account.CreatedAt {
			return links, invalid("BAAS account link")
		}
		value := *account
		links.Account = &value
	}
	if friendCode != nil {
		value := *friendCode
		value.ID = strings.TrimPrefix(value.ID, "SW-")
		if account == nil || !validFriendCode(value.ID) || value.CreatedAt < 0 ||
			value.UpdatedAt < value.CreatedAt || value.RegenerableAt < 0 {
			return BaaSLinks{}, invalid("BAAS friend code")
		}
		links.FriendCode = &value
	}
	return links, nil
}

func validFriendCode(value string) bool {
	if len(value) != 14 || value[4] != '-' || value[9] != '-' {
		return false
	}
	return strings.Trim(value[:4]+value[5:9]+value[10:], "0123456789") == ""
}

type ProfilePatch struct {
	Op    string `json:"op"`
	Path  string `json:"path"`
	Value string `json:"value"`
}

// ApplyBaaSProfilePatch builds the new profile for a caller whose ownership
// was already verified. Preserve the original state when any entry is invalid.
func ApplyBaaSProfilePatch(ownerID, userID string, profile BaaSProfile, ops []ProfilePatch, updatedAt int64) (BaaSProfile, error) {
	if err := requireOwner(ownerID, userID); err != nil {
		return profile, err
	}
	if len(ops) == 0 || len(ops) > 2 || updatedAt <= 0 || updatedAt < profile.NicknameUpdatedAt {
		return profile, invalid("BAAS profile patch")
	}
	next := profile
	seen := make(map[string]bool, len(ops))
	for _, op := range ops {
		if op.Op != "replace" || seen[op.Path] {
			return profile, invalid("BAAS profile patch operation")
		}
		seen[op.Path] = true
		switch op.Path {
		case "/nickname":
			if !validText(op.Value, 32, false) {
				return profile, invalid("BAAS profile nickname")
			}
			next.Nickname, next.NicknameUpdatedAt = op.Value, updatedAt
		case "/extras/self/nxAccount":
			if !validText(op.Value, maxProfileBlobBytes, false) {
				return profile, invalid("BAAS profile blob")
			}
			next.AccountBlob = op.Value
		default:
			return profile, invalid("BAAS profile patch path")
		}
	}
	return next, nil
}

type AvatarContent struct {
	URL    string `json:"url"`
	Format string `json:"format"`
}

type AvatarUploadResponse struct {
	Content AvatarContent `json:"content"`
}

// BuildAvatarUpload renders the result for an upload the caller has validated
// and stored. The same timestamp supplies BAAS thumbnail cache invalidation.
func BuildAvatarUpload(ownerID, userID, cdnOrigin string, uploadedAt int64) (AvatarUploadResponse, error) {
	if err := requireOwner(ownerID, userID); err != nil {
		return AvatarUploadResponse{}, err
	}
	if uploadedAt <= 0 {
		return AvatarUploadResponse{}, invalid("BAAS avatar timestamp")
	}
	uri, err := thumbnailURL(cdnOrigin, "1", userID, uploadedAt)
	if err != nil {
		return AvatarUploadResponse{}, err
	}
	return AvatarUploadResponse{Content: AvatarContent{URL: uri, Format: "jpg"}}, nil
}

func thumbnailURL(origin, size, userID string, uploadedAt int64) (string, error) {
	base, err := ownedBase(origin)
	if err != nil {
		return "", err
	}
	if uploadedAt == 0 {
		userID = "default"
	}
	uri := base.JoinPath(size, userID)
	if uploadedAt > 0 {
		uri.RawQuery = fmt.Sprintf("v=%d", uploadedAt)
	}
	if value := uri.String(); len(value) < 256 {
		return value, nil
	}
	return "", errors.New("avatar URL exceeds the protocol limit")
}
