// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"bytes"
	"crypto/tls"
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"
)

func testHello(t *testing.T, name string) []byte {
	t.Helper()
	client, peer := net.Pipe()
	defer client.Close()
	defer peer.Close()
	_ = client.SetDeadline(time.Now().Add(time.Second))
	_ = peer.SetDeadline(time.Now().Add(time.Second))
	go func() {
		_ = tls.Client(client, &tls.Config{ServerName: name, MinVersion: tls.VersionTLS12}).Handshake()
	}()

	var header [5]byte
	if _, err := io.ReadFull(peer, header[:]); err != nil {
		t.Fatal(err)
	}
	payload := make([]byte, int(binary.BigEndian.Uint16(header[3:])))
	if _, err := io.ReadFull(peer, payload); err != nil {
		t.Fatal(err)
	}
	return append(header[:], payload...)
}

func TestServerNameASCIIOnly(t *testing.T) {
	for _, test := range []struct {
		name, want string
		allowed    bool
	}{
		{"Game-Pico-Park.roomcircuit.test", "game-pico-park.roomcircuit.test", true},
		{"game-pico-parK.roomcircuit.test", "", false},
	} {
		wire := testHello(t, test.name)
		name, _, err := hello(bytes.NewReader(wire))
		if (err == nil) != test.allowed || err == nil && name != test.want {
			t.Fatalf("unexpected TLS name verdict for %q: %q, %v", test.name, name, err)
		}
	}
}

func TestFixedLoopbackPortsAndNoSelfRoutes(t *testing.T) {
	for _, test := range []struct {
		name, listen, backend string
		allowed               bool
	}{
		{"fixed other port", "127.0.0.1:443", "127.0.0.1:39282", true},
		{"fixed IPv6 loopback", "127.0.0.1:443", "[::1]:39282", true},
		{"empty port", "127.0.0.1:443", "127.0.0.1:", false},
		{"zero alias", "127.0.0.1:443", "127.0.0.1:00", false},
		{"service name", "127.0.0.1:443", "127.0.0.1:https", false},
		{"exact self route", "127.0.0.1:443", "127.0.0.1:443", false},
		{"IPv4 wildcard self route", "0.0.0.0:443", "127.0.0.1:443", false},
		{"IPv6 wildcard self route", "[::]:443", "[::1]:443", false},
		{"unspecified wildcard self route", ":443", "127.0.0.1:443", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			config := configuration{Listen: test.listen, NoSNI: route{Address: "127.0.0.1:39443"},
				Routes: map[string]route{"account.roomcircuit.test": {Address: test.backend}}}
			if err := validate(config); (err == nil) != test.allowed {
				t.Fatalf("unexpected routing configuration verdict: %v", err)
			}
		})
	}
}

func TestBridgeDeliversResponseAfterClientHalfClose(t *testing.T) {
	wire := testHello(t, "account.roomcircuit.test")
	request := append(wire, []byte("test-body-after-client-hello")...)
	backend, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer backend.Close()
	front, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer front.Close()
	const response = "response-after-tcp-eof"
	go func() {
		conn, err := backend.AcceptTCP()
		if err != nil {
			return
		}
		defer conn.Close()
		_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
		received, err := io.ReadAll(io.LimitReader(conn, int64(len(request)+1)))
		if err == nil && bytes.Equal(received, request) {
			_, _ = io.WriteString(conn, response)
		}
	}()
	service := route{Address: backend.Addr().String()}
	go serve(front, configuration{Listen: front.Addr().String(), NoSNI: service, Routes: map[string]route{"account.roomcircuit.test": service}})

	client, err := net.DialTCP("tcp4", nil, front.Addr().(*net.TCPAddr))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	_ = client.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err = client.Write(request); err != nil {
		t.Fatal(err)
	}
	if err = client.CloseWrite(); err != nil {
		t.Fatal(err)
	}
	result, err := io.ReadAll(io.LimitReader(client, int64(len(response)+1)))
	if err != nil || string(result) != response {
		t.Fatalf("the upstream response after TCP EOF was lost: %v", err)
	}
}
