package main

import (
	"crypto/rand"
	"crypto/rsa"
	"testing"
	"time"
)

func TestSignedGameIdentityChecksIssuerPurposeAudienceAndExpiry(t *testing.T) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	s := &service{issuerURL: "https://127.0.0.1:39280", signing: key}
	now := time.Now()
	valid := claims{
		Issuer: s.issuerURL, Audience: scope, Purpose: "id_token",
		Subject: "0000000000001234", ID: "fixture", Expires: now.Add(time.Minute).Unix(),
	}
	for _, row := range []struct {
		name string
		edit func(*claims)
		ok   bool
	}{
		{"valid", func(*claims) {}, true},
		{"wrong issuer", func(c *claims) { c.Issuer = "https://127.0.0.1:43810" }, false},
		{"wrong audience", func(c *claims) { c.Audience = "unrelated-application" }, false},
		{"access token", func(c *claims) { c.Purpose = "token" }, false},
		{"expired", func(c *claims) { c.Expires = now.Unix() }, false},
	} {
		t.Run(row.name, func(t *testing.T) {
			input := valid
			row.edit(&input)
			signed, err := s.sign(input)
			if err != nil {
				t.Fatal(err)
			}
			if _, ok := s.verify(signed, now); ok != row.ok {
				t.Fatalf("verify returned %t, want %t", ok, row.ok)
			}
		})
	}
}
