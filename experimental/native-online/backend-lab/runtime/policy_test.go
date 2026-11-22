// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"bytes"
	"net/http"
	"strings"
	"testing"
	"time"

	switchnex "github.com/openpak/switch-nex"
)

func TestRoomCircuitRuntimeConfiguration(t *testing.T) {
	key, ticket := strings.Repeat("1a", 32), strings.Repeat("2b", 32)
	const loopback = "http://127.0.0.1:39283"
	const titles = "mario-party-superstars,pico-park"
	cases := []struct {
		name                 string
		identity, k, secret  string
		station, bind, proxy string
		profiles             string
		enforce, accepted    bool
	}{
		{"both titles on loopback", loopback, key, ticket, "127.0.0.1", "127.0.0.1", "", titles, true, true},
		{"explicit auth proxy", loopback, key, ticket, "192.0.2.1", "0.0.0.0", "10.0.0.2", "pico-park", true, true},
		{"identity disabled", loopback, key, ticket, "127.0.0.1", "127.0.0.1", "", titles, false, false},
		{"no identity URL", "", key, ticket, "127.0.0.1", "127.0.0.1", "", titles, true, false},
		{"nonlocal URL", "http://192.0.2.1:20000", key, ticket, "127.0.0.1", "127.0.0.1", "", titles, true, false},
		{"missing key", loopback, "", ticket, "127.0.0.1", "127.0.0.1", "", titles, true, false},
		{"public test ticket password", loopback, key, "openpak-switch-nex-secure", "127.0.0.1", "127.0.0.1", "", titles, true, false},
		{"same secret reused", loopback, key, key, "127.0.0.1", "127.0.0.1", "", titles, true, false},
		{"missing station address", loopback, key, ticket, "", "127.0.0.1", "", titles, true, false},
		{"no implicit fleet", loopback, key, ticket, "127.0.0.1", "127.0.0.1", "", "", true, false},
		{"unknown title", loopback, key, ticket, "127.0.0.1", "127.0.0.1", "", titles + ",unknown", true, false},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			_, err := runtimeConfiguration(test.enforce, test.identity, test.k, test.secret, test.station, test.bind, test.proxy, test.profiles)
			if (err == nil) != test.accepted {
				t.Fatalf("unexpected configuration assessment: %v", err)
			}
		})
	}
	secret := []byte(strings.Repeat("test-only", 4))
	if bytes.Equal(runtimeTicketPassword(secret, "pico-park"), runtimeTicketPassword(secret, "mario-party-superstars")) {
		t.Fatal("the same server secret yielded the same password across titles")
	}
	certificate, _ := ownTLS(t)
	if err := checkRuntimeCertificate(certificate.Certificates[0], "127.0.0.1"); err != nil {
		t.Fatalf("explicit station address is present in certificate: %v", err)
	}
	if err := checkRuntimeCertificate(certificate.Certificates[0], "127.0.0.2"); err == nil {
		t.Fatal("startup accepts a certificate for a different secure station")
	}
	privateIdentity := newIdentity(loopback, key)
	if privateIdentity.client.CheckRedirect(nil, nil) != http.ErrUseLastResponse {
		t.Fatal("identity requests could forward the private key to a redirect target")
	}
}

func TestRoomCircuitRuntimeTicketWindow(t *testing.T) {
	now := time.Date(2026, 1, 1, 0, 1, 0, 0, time.UTC)
	for _, test := range []struct {
		offset time.Duration
		valid  bool
	}{
		{0, true}, {-runtimeTicketMaxAge, true}, {-runtimeTicketMaxAge - time.Second, false},
		{runtimeTicketSkew, true}, {runtimeTicketSkew + time.Second, false},
	} {
		issued := switchnex.MakeDateTime(now.Add(test.offset))
		if runtimeTicketWithinWindow(issued, now) != test.valid {
			t.Fatalf("unexpected ticket age check for %s", test.offset)
		}
	}

	sourceKey, targetKey := ownSecret(t, 32), ownSecret(t, 16)
	ticket, _, err := switchnex.Mint(sourceKey, 77, 2, targetKey)
	if err != nil {
		t.Fatal(err)
	}
	clientTicket, err := switchnex.DecryptClientTicket(ticket, sourceKey)
	if err != nil {
		t.Fatal(err)
	}
	serverTicket, err := switchnex.DecryptServerTicket(clientTicket.Internal, targetKey)
	if err != nil {
		t.Fatal(err)
	}
	current := time.Now()
	if !runtimeTicketWithinWindow(serverTicket.Timestamp, current) ||
		runtimeTicketWithinWindow(serverTicket.Timestamp, current.Add(runtimeTicketMaxAge+time.Second)) {
		t.Fatal("a genuine own-realm ticket did not expire at the configured age")
	}
}
