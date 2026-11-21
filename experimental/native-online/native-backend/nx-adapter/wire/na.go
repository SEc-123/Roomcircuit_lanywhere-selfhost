// SPDX-License-Identifier: AGPL-3.0-only
// Response field layout adapted from OpenPak nx-baas cf93fd71,
// internal/nxaccount/na.go. Values come from this realm's verified state.
package wire

import (
	"net/mail"
	"strings"
	"time"
)

type NATermAgreement struct {
	AgreedAt int64  `json:"agreedAt"`
	Country  string `json:"country"`
	Version  int    `json:"version"`
}

type NATermAgreements struct {
	EULA          *NATermAgreement `json:"EULA,omitempty"`
	PrivacyPolicy *NATermAgreement `json:"privacyPolicy,omitempty"`
}

type NATimezone struct {
	ID               string `json:"id"`
	Name             string `json:"name"`
	UTCOffset        string `json:"utcOffset"`
	UTCOffsetSeconds int    `json:"utcOffsetSeconds"`
}

type NAEditable struct {
	Admin bool `json:"admin"`
	Self  bool `json:"self"`
}

type NAAnalyticsPermission struct {
	Editable      NAEditable `json:"editable"`
	Permitted     bool       `json:"permitted"`
	UpdatedAt     int64      `json:"updatedAt"`
	UserConfirmed bool       `json:"userConfirmed"`
}

type NAAnalyticsPermissions struct {
	DataCollection   NAAnalyticsPermission `json:"dataCollection"`
	InternalAnalysis NAAnalyticsPermission `json:"internalAnalysis"`
	TargetMarketing  NAAnalyticsPermission `json:"targetMarketing"`
}

type NAOptIn struct {
	OptedIn   bool  `json:"optedIn"`
	UpdatedAt int64 `json:"updatedAt"`
}

type NAEachEmailOptIn struct {
	Deals  NAOptIn `json:"deals"`
	Survey NAOptIn `json:"survey"`
}

type NAProfilePreferences struct {
	AnalyticsOptedIn       bool                   `json:"analyticsOptedIn"`
	AnalyticsOptedInAt     int64                  `json:"analyticsOptedInUpdatedAt"`
	AnalyticsPermissions   NAAnalyticsPermissions `json:"analyticsPermissions"`
	ClientFriendsOptedIn   bool                   `json:"clientFriendsOptedIn"`
	ClientFriendsOptedInAt int64                  `json:"clientFriendsOptedInUpdatedAt"`
	EmailOptedIn           bool                   `json:"emailOptedIn"`
	EmailOptedInAt         int64                  `json:"emailOptedInUpdatedAt"`
	EachEmailOptedIn       NAEachEmailOptIn       `json:"eachEmailOptedIn"`
}

type NAAccountSummary struct {
	Nintendo NAMembershipSummary `json:"nintendo"`
}

type NAMembershipSummary struct {
	HasMembership bool `json:"hasMembership"`
}

type NAProfileInput struct {
	ID, AccessSubject, Nickname, Email, Country, Birthday, IconURI, Language, Gender string
	EmailVerified, IsChild, PhoneNumberEnabled, TermsAgreementRequired               bool
	// UsernameOnly is set only after the caller verifies the separate local
	// username-account policy. It does not imply an email has been verified.
	UsernameOnly bool
	// VerifiedRealmMembership represents only a live independent game grant
	// verified by the caller for this profile. A compatibility field name
	// does not change the issuer or grant any external subscription.
	VerifiedRealmMembership bool
	CreatedAt, UpdatedAt    int64
	Terms                   NATermAgreements
	Timezone                NATimezone
	Preferences             NAProfilePreferences
}

type NAProfile struct {
	ID                     string           `json:"id"`
	Nickname               string           `json:"nickname"`
	Email                  string           `json:"email"`
	EmailVerified          bool             `json:"emailVerified"`
	ScreenName             string           `json:"screenName"`
	Birthday               string           `json:"birthday"`
	Country                string           `json:"country"`
	CreatedAt              int64            `json:"createdAt"`
	UpdatedAt              int64            `json:"updatedAt"`
	Gender                 string           `json:"gender"`
	Language               string           `json:"language"`
	IconURI                string           `json:"iconUri"`
	IsChild                bool             `json:"isChild"`
	PhoneNumberEnabled     bool             `json:"phoneNumberEnabled"`
	TermsAgreementRequired bool             `json:"termsAgreementRequired"`
	AgreedTerms            NATermAgreements `json:"agreedTerms"`
	Timezone               NATimezone       `json:"timezone"`
	Summary                NAAccountSummary `json:"summary"`
	// This realm does not project external platform identities or a region.
	Links   struct{} `json:"links"`
	LoginID *string  `json:"loginId"`
	Region  *string  `json:"region"`
	NAProfilePreferences
}

func BuildNAProfile(input NAProfileInput) (NAProfile, error) {
	if err := requireOwner(input.AccessSubject, input.ID); err != nil {
		return NAProfile{}, err
	}
	if !validText(input.Nickname, 32, false) || input.CreatedAt < 0 || input.UpdatedAt < input.CreatedAt ||
		!validText(input.Language, 24, true) || !validText(input.Gender, 32, true) {
		return NAProfile{}, invalid("account profile")
	}
	screenName := ""
	if input.UsernameOnly {
		if input.Email != "" || input.EmailVerified ||
			(input.Country != "" && !validCountry(input.Country)) ||
			(input.Birthday != "" && !validDate(input.Birthday)) {
			return NAProfile{}, invalid("username account profile")
		}
		// No unprovided address, birth date or location is invented.
		screenName = input.Nickname
	} else {
		if !input.EmailVerified || !validCountry(input.Country) || !validDate(input.Birthday) {
			return NAProfile{}, invalid("verified account profile")
		}
		var err error
		screenName, err = maskAddress(input.Email)
		if err != nil {
			return NAProfile{}, err
		}
	}
	if input.IconURI != "" {
		if !validText(input.IconURI, 2048, false) {
			return NAProfile{}, invalid("account icon URL")
		}
		if _, err := ownedHTTPS(input.IconURI); err != nil {
			return NAProfile{}, err
		}
	}
	terms, err := copyTerms(input.Terms)
	if err != nil {
		return NAProfile{}, err
	}
	if !validTimezone(input.Timezone) || !validPreferences(input.Preferences) {
		return NAProfile{}, invalid("account profile preferences")
	}
	gender := input.Gender
	if gender == "" {
		gender = "unknown"
	}
	return NAProfile{
		ID: input.ID, Nickname: input.Nickname, Email: input.Email, EmailVerified: input.EmailVerified,
		ScreenName: screenName, Birthday: input.Birthday, Country: input.Country, CreatedAt: input.CreatedAt,
		UpdatedAt: input.UpdatedAt, Gender: gender, Language: input.Language, IconURI: input.IconURI,
		IsChild: input.IsChild, PhoneNumberEnabled: input.PhoneNumberEnabled,
		TermsAgreementRequired: input.TermsAgreementRequired, AgreedTerms: terms, Timezone: input.Timezone,
		Summary:              NAAccountSummary{Nintendo: NAMembershipSummary{HasMembership: input.VerifiedRealmMembership}},
		NAProfilePreferences: input.Preferences,
	}, nil
}

func maskAddress(value string) (string, error) {
	if !validText(value, 254, false) {
		return "", invalid("verified email")
	}
	address, err := mail.ParseAddress(value)
	if err != nil || address.Address != value || address.Name != "" {
		return "", invalid("verified email")
	}
	name, domain, hasSeparator := strings.Cut(value, "@")
	if !hasSeparator || name == "" || domain == "" || strings.Contains(domain, "@") {
		return "", invalid("verified email")
	}
	local, host := []rune(name), []rune(domain)
	return string(local[:min(2, len(local))]) + "•••@" + string(host[:1]) + "••••", nil
}

func copyTerms(terms NATermAgreements) (NATermAgreements, error) {
	var output NATermAgreements
	for _, item := range []struct {
		input  *NATermAgreement
		output **NATermAgreement
	}{{terms.EULA, &output.EULA}, {terms.PrivacyPolicy, &output.PrivacyPolicy}} {
		if item.input == nil {
			continue
		}
		value := *item.input
		if value.AgreedAt <= 0 || value.Version <= 0 || !validCountry(value.Country) {
			return NATermAgreements{}, invalid("account term agreement")
		}
		*item.output = &value
	}
	return output, nil
}

func validTimezone(value NATimezone) bool {
	if value == (NATimezone{}) {
		return true
	}
	if !validText(value.ID, 128, false) || !validText(value.Name, 128, false) || len(value.UTCOffset) != 6 ||
		value.UTCOffsetSeconds < -14*3600 || value.UTCOffsetSeconds > 14*3600 {
		return false
	}
	offset, err := time.Parse("-07:00", value.UTCOffset)
	if err != nil || offset.Format("-07:00") != value.UTCOffset {
		return false
	}
	_, seconds := offset.Zone()
	return seconds == value.UTCOffsetSeconds
}

func validPreferences(value NAProfilePreferences) bool {
	return value.AnalyticsOptedInAt >= 0 && value.ClientFriendsOptedInAt >= 0 && value.EmailOptedInAt >= 0 &&
		value.AnalyticsPermissions.DataCollection.UpdatedAt >= 0 && value.AnalyticsPermissions.InternalAnalysis.UpdatedAt >= 0 &&
		value.AnalyticsPermissions.TargetMarketing.UpdatedAt >= 0 && value.EachEmailOptedIn.Deals.UpdatedAt >= 0 &&
		value.EachEmailOptedIn.Survey.UpdatedAt >= 0
}
