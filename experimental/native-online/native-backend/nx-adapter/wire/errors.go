// SPDX-License-Identifier: AGPL-3.0-only
// Error layout adapted from OpenPak nx-baas cf93fd71, internal/httpx/httpx.go.
package wire

import (
	"net/url"
	"strconv"
	"strings"
)

type BaaSError struct {
	Status    int    `json:"status"`
	ErrorCode string `json:"errorCode"`
	Title     string `json:"title"`
	Detail    string `json:"detail"`
	Instance  string `json:"instance"`
	Type      string `json:"type"`
}

func BuildBaaSError(origin string, status int, code, title, detail, instance string) (BaaSError, error) {
	base, err := ownedBase(origin)
	if err != nil {
		return BaaSError{}, err
	}
	if status < 400 || status > 599 || code == "" || len(code) > 64 ||
		strings.Trim(code, "abcdefghijklmnopqrstuvwxyz0123456789_") != "" ||
		!validText(title, 128, false) || !validText(detail, 1024, true) {
		return BaaSError{}, invalid("BAAS error description")
	}
	if !validText(instance, 2048, false) || !strings.HasPrefix(instance, "/") || strings.HasPrefix(instance, "//") ||
		strings.ContainsAny(instance, "?#\\") {
		return BaaSError{}, invalid("BAAS error route")
	}
	if _, err := url.ParseRequestURI(instance); err != nil {
		return BaaSError{}, invalid("BAAS error route")
	}
	problem := base.JoinPath("errors", "1.0.0", strconv.Itoa(status), code)
	return BaaSError{Status: status, ErrorCode: code, Title: title, Detail: detail, Instance: instance, Type: problem.String()}, nil
}
