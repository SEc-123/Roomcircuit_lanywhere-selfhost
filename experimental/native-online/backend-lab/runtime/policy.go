// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"bufio"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"net/url"
	"strconv"
	"strings"
	"time"

	switchnex "github.com/openpak/switch-nex"
)

const (
	runtimeTicketMaxAge = 2 * time.Hour
	runtimeTicketSkew   = time.Minute
)

// These application IDs identify the actual title whose NEX endpoint is in
// use. They must be checked against the authenticated adapter response.
func runtimeTitleID(name string) string {
	switch name {
	case "mario-party-superstars":
		return "01006fe013472000"
	case "pico-park":
		return "01004ea00df70000"
	default:
		return ""
	}
}

func runtimeConfiguration(require bool, identity, key, ticket, station, bind, proxy, selected string) ([]byte, error) {
	if !require {
		return nil, errors.New("identity enforcement is required")
	}
	issuer, err := url.Parse(identity)
	if err != nil || issuer.Scheme != "http" || issuer.Hostname() != "127.0.0.1" || issuer.User != nil ||
		(issuer.Path != "" && issuer.Path != "/") || issuer.RawQuery != "" || issuer.ForceQuery || issuer.Fragment != "" {
		return nil, errors.New("identity URL must be a private loopback HTTP origin")
	}
	port, err := strconv.ParseUint(issuer.Port(), 10, 16)
	if err != nil || port == 0 {
		return nil, errors.New("identity URL requires a valid port")
	}
	if decoded, err := hex.DecodeString(key); err != nil || len(decoded) != 32 {
		return nil, errors.New("NX_INTERNAL_KEY must contain 32 random bytes encoded as hex")
	}
	secret, err := hex.DecodeString(ticket)
	if err != nil || len(secret) != 32 || strings.EqualFold(key, ticket) {
		return nil, errors.New("RC_NEX_TICKET_KEY must contain a separate 32-byte random secret encoded as hex")
	}
	stationAddr, err := netip.ParseAddr(station)
	if err != nil || !stationAddr.Is4() || !stationAddr.IsGlobalUnicast() && !stationAddr.IsLoopback() {
		return nil, errors.New("the advertised station must be an explicit unicast IPv4 address")
	}
	if ip, err := netip.ParseAddr(bind); err != nil || !ip.Is4() || ip.IsMulticast() || ip == netip.AddrFrom4([4]byte{255, 255, 255, 255}) {
		return nil, errors.New("the bind address must be an IPv4 interface address")
	}
	if proxy != "" {
		ip, err := netip.ParseAddr(proxy)
		if err != nil || !ip.Is4() || !ip.IsLoopback() && !ip.IsPrivate() {
			return nil, errors.New("the PROXY peer must be a specific private IPv4 address")
		}
	}
	if len(splitCSV(selected)) == 0 {
		return nil, errors.New("choose at least one supported title explicitly with -only")
	}
	for _, name := range splitCSV(selected) {
		if name != "mario-party-superstars" && name != "pico-park" {
			return nil, fmt.Errorf("title %q is outside this realm's validated profiles", name)
		}
	}
	return secret, nil
}

func runtimeTicketPassword(secret []byte, title string) []byte {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte("roomcircuit-nex-ticket-v1/" + title))
	return mac.Sum(nil)
}

func runtimeTicketWithinWindow(issued uint64, now time.Time) bool {
	return issued >= switchnex.MakeDateTime(now.Add(-runtimeTicketMaxAge)) &&
		issued <= switchnex.MakeDateTime(now.Add(runtimeTicketSkew))
}

func validateRuntimeTicket(ticket *switchnex.ServerTicket, ids *identity) error {
	if !runtimeTicketWithinWindow(ticket.Timestamp, time.Now()) {
		return errors.New("secure ticket outside its admission window")
	}
	if ticket.SourcePID == 0 || ids.stillAllowed(ticket.SourcePID) != nil {
		return errors.New("secure ticket requires an active game admission")
	}
	return nil
}

func checkRuntimeCertificate(pair tls.Certificate, station string) error {
	if len(pair.Certificate) == 0 {
		return errors.New("server certificate missing")
	}
	leaf, err := x509.ParseCertificate(pair.Certificate[0])
	if err != nil {
		return err
	}
	if now := time.Now(); now.Before(leaf.NotBefore) || now.After(leaf.NotAfter) {
		return errors.New("server certificate outside its validity period")
	}
	return leaf.VerifyHostname(station)
}

func listenRuntimeTitle(address string, cfg *tls.Config, trustedProxy string) (net.Listener, error) {
	listener, err := net.Listen("tcp", address)
	if err != nil {
		return nil, err
	}
	if trustedProxy != "" {
		listener = runtimeProxyListener{Listener: listener, trusted: net.ParseIP(trustedProxy)}
	}
	return tls.NewListener(listener, cfg), nil
}

type runtimeProxyListener struct {
	net.Listener
	trusted net.IP
}

func (l runtimeProxyListener) Accept() (net.Conn, error) {
	for {
		connection, err := l.Listener.Accept()
		if err != nil {
			return nil, err
		}
		peer, ok := connection.RemoteAddr().(*net.TCPAddr)
		if !ok || !l.trusted.Equal(peer.IP) {
			return connection, nil
		}
		buffer := bufio.NewReader(connection)
		_ = connection.SetReadDeadline(time.Now().Add(proxyHeaderWait))
		original, err := readProxyHeader(buffer)
		_ = connection.SetReadDeadline(time.Time{})
		if err == nil {
			return &proxiedConn{Conn: connection, r: buffer, remote: original}, nil
		}
		_ = connection.Close()
	}
}
