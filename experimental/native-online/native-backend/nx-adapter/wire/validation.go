// SPDX-License-Identifier: AGPL-3.0-only
package wire

import (
	"errors"
	"fmt"
	"net/url"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

var (
	ErrInvalidField  = errors.New("invalid native wire field")
	ErrOwnerMismatch = errors.New("wire profile does not belong to the verified owner")
)

func invalid(field string) error { return fmt.Errorf("%w: %s", ErrInvalidField, field) }

func validHexID(value string, width int) bool {
	return len(value) == width && strings.Trim(value, "0123456789abcdef") == "" && strings.Trim(value, "0") != ""
}

func validDeviceID(value string) bool {
	return validHexID(value, 16) || validHexID(value, 32)
}

func validText(value string, maxBytes int, allowEmpty bool) bool {
	if value == "" {
		return allowEmpty
	}
	return len(value) <= maxBytes && utf8.ValidString(value) && strings.TrimSpace(value) != "" &&
		strings.IndexFunc(value, unicode.IsControl) == -1
}

func stableToken(value string) bool {
	return value != "" && len(value) <= 128 && strings.Trim(value,
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._~-") == ""
}

func validCountry(value string) bool {
	return len(value) == 2 && value[0] >= 'A' && value[0] <= 'Z' && value[1] >= 'A' && value[1] <= 'Z'
}

func validDate(value string) bool {
	date, err := time.Parse("2006-01-02", value)
	return err == nil && date.Year() > 0
}

func requireOwner(ownerID, userID string) error {
	if !validHexID(ownerID, 16) || !validHexID(userID, 16) || ownerID != userID {
		return ErrOwnerMismatch
	}
	return nil
}

func ownedHTTPS(value string) (*url.URL, error) {
	u, err := url.Parse(value)
	if err != nil || u.Scheme != "https" || u.Hostname() == "" || u.User != nil ||
		u.Fragment != "" || strings.Contains(value, "#") || u.Opaque != "" || u.ForceQuery {
		return nil, invalid("owned HTTPS URL")
	}
	if port := u.Port(); port != "" {
		if number, err := strconv.ParseUint(port, 10, 16); err != nil || number == 0 {
			return nil, invalid("owned HTTPS port")
		}
	}
	return u, nil
}

func ownedBase(value string) (*url.URL, error) {
	u, err := ownedHTTPS(value)
	if err != nil || u.RawQuery != "" {
		return nil, invalid("owned HTTPS base URL")
	}
	return u, nil
}
