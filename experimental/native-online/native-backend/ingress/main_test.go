package main

import (
	"bufio"
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/pem"
	"fmt"
	"io"
	"math/big"
	"net"
	"strings"
	"testing"
	"time"
)

type bufferedConn struct {
	net.Conn
	reader *bufio.Reader
}

func (c bufferedConn) Read(b []byte) (int, error) { return c.reader.Read(b) }

func TestProxyV1PreservesTLS(t *testing.T) {
	certificate, _, pool := certs(t)
	backend, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer backend.Close()
	header := make(chan string, 1)
	go func() {
		conn, e := backend.Accept()
		if e != nil {
			return
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		reader := bufio.NewReader(conn)
		line, e := reader.ReadString('\n')
		if e != nil {
			return
		}
		header <- line
		tlsConn := tls.Server(bufferedConn{conn, reader}, &tls.Config{Certificates: []tls.Certificate{certificate}, MinVersion: tls.VersionTLS12})
		defer tlsConn.Close()
		b := make([]byte, 4)
		if _, e = io.ReadFull(tlsConn, b); e == nil {
			tlsConn.Write(b)
		}
	}()
	front, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer front.Close()
	c := configuration{Listen: front.Addr().String(), NoSNI: route{Address: backend.Addr().String()}, Routes: map[string]route{"account.roomcircuit.test": {Address: backend.Addr().String(), ProxyV1: true}}}
	go serve(front, c)
	client, e := tls.DialWithDialer(&net.Dialer{Timeout: 3 * time.Second}, "tcp", front.Addr().String(), &tls.Config{RootCAs: pool, ServerName: "account.roomcircuit.test", MinVersion: tls.VersionTLS12})
	if e != nil {
		t.Fatal(e)
	}
	defer client.Close()
	client.SetDeadline(time.Now().Add(3 * time.Second))
	if _, e = client.Write([]byte("PING")); e != nil {
		t.Fatal(e)
	}
	b := make([]byte, 4)
	if _, e = io.ReadFull(client, b); e != nil || string(b) != "PING" {
		t.Fatalf("response=%q err=%v", b, e)
	}
	line := <-header
	src := client.NetConn().LocalAddr().(*net.TCPAddr)
	dst := front.Addr().(*net.TCPAddr)
	want := fmt.Sprintf("PROXY TCP4 127.0.0.1 127.0.0.1 %d %d\r\n", src.Port, dst.Port)
	if line != want {
		t.Fatalf("header=%q want=%q", line, want)
	}
}

func certs(t *testing.T) (tls.Certificate, tls.Certificate, *x509.CertPool) {
	t.Helper()
	k, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	ca := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "own test CA"}, NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour), IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign}
	der, err := x509.CreateCertificate(rand.Reader, ca, ca, &k.PublicKey, k)
	if err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	pool.AddCert(mustCert(t, der))
	leaf := func(n int64, client bool) tls.Certificate {
		lk, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		x := &x509.Certificate{SerialNumber: big.NewInt(n), Subject: pkix.Name{CommonName: "own leaf"}, NotBefore: ca.NotBefore, NotAfter: ca.NotAfter, DNSNames: []string{"account.roomcircuit.test", "device.roomcircuit.test"}, KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
		if client {
			x.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
		}
		d, e := x509.CreateCertificate(rand.Reader, x, ca, &lk.PublicKey, k)
		if e != nil {
			t.Fatal(e)
		}
		key, _ := x509.MarshalECPrivateKey(lk)
		c, e := tls.X509KeyPair(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: d}), pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: key}))
		if e != nil {
			t.Fatal(e)
		}
		return c
	}
	return leaf(2, false), leaf(3, true), pool
}
func mustCert(t *testing.T, b []byte) *x509.Certificate {
	c, e := x509.ParseCertificate(b)
	if e != nil {
		t.Fatal(e)
	}
	return c
}

func TestTransparentTLSAndDeviceProof(t *testing.T) {
	server, device, pool := certs(t)
	backend := func(require bool) net.Listener {
		config := &tls.Config{Certificates: []tls.Certificate{server}, MinVersion: tls.VersionTLS12}
		if require {
			config.ClientAuth = tls.RequireAndVerifyClientCert
			config.ClientCAs = pool
		}
		l, e := tls.Listen("tcp", "127.0.0.1:0", config)
		if e != nil {
			t.Fatal(e)
		}
		t.Cleanup(func() { l.Close() })
		go func() {
			for {
				c, e := l.Accept()
				if e != nil {
					return
				}
				go func() {
					defer c.Close()
					c.SetDeadline(time.Now().Add(3 * time.Second))
					if e := c.(*tls.Conn).Handshake(); e != nil {
						return
					}
					buf := make([]byte, 4)
					if _, e := io.ReadFull(c, buf); e == nil {
						c.Write(buf)
					}
				}()
			}
		}()
		return l
	}
	web, auth := backend(false), backend(true)
	l, e := net.Listen("tcp", "127.0.0.1:0")
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { l.Close() })
	c := configuration{Listen: l.Addr().String(), NoSNI: route{Address: web.Addr().String()}, Routes: map[string]route{"account.roomcircuit.test": {Address: web.Addr().String()}, "device.roomcircuit.test": {Address: auth.Addr().String()}}}
	if e = validate(c); e != nil {
		t.Fatal(e)
	}
	go serve(l, c)
	for _, tc := range []struct {
		name       string
		device, ok bool
	}{{"account.roomcircuit.test", false, true}, {"device.roomcircuit.test", true, true}, {"device.roomcircuit.test", false, false}, {"unknown.roomcircuit.test", false, false}} {
		t.Run(tc.name+map[bool]string{true: "-device", false: "-anonymous"}[tc.device], func(t *testing.T) {
			cfg := &tls.Config{RootCAs: pool, ServerName: tc.name, MinVersion: tls.VersionTLS12}
			if tc.device {
				cfg.Certificates = []tls.Certificate{device}
			}
			x, err := tls.DialWithDialer(&net.Dialer{Timeout: 3 * time.Second}, "tcp", l.Addr().String(), cfg)
			if err == nil {
				defer x.Close()
				x.SetDeadline(time.Now().Add(3 * time.Second))
				_, err = x.Write([]byte("PING"))
				if err == nil {
					b := make([]byte, 4)
					_, err = io.ReadFull(x, b)
					if err == nil && !bytes.Equal(b, []byte("PING")) {
						t.Fatal("wrong backend response")
					}
				}
			}
			if (err == nil) != tc.ok {
				t.Fatalf("success=%v want=%v error=%v", err == nil, tc.ok, err)
			}
		})
	}
}

func TestMalformedHelloAndRouteConfig(t *testing.T) {
	for _, b := range [][]byte{nil, {22, 3, 1, 255, 255}, {23, 3, 3, 0, 1, 0}, {22, 3, 3, 0, 4, 2, 0, 0, 0}} {
		if _, _, e := hello(bytes.NewReader(b)); e == nil {
			t.Fatal("accepted malformed hello")
		}
	}
	c := configuration{Listen: "127.0.0.1:443", NoSNI: route{Address: "127.0.0.1:39443"}, Routes: map[string]route{"account.roomcircuit.test": {Address: "127.0.0.1:39282"}}}
	for _, bad := range []string{"49.233.147.207:443", "example.com:443", "127.0.0.1:0", "127.0.0.1:-1"} {
		c.Routes["account.roomcircuit.test"] = route{Address: bad}
		if validate(c) == nil {
			t.Fatal("accepted non-private backend", bad)
		}
	}
	if validName("*.roomcircuit.test") || validName(strings.Repeat("a", 254)) || validName("accounts.roomcircuit.test/evil") {
		t.Fatal("accepted invalid hostname")
	}
}

func TestNoSNIDisabled(t *testing.T) {
	backend, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer backend.Close()
	front, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer front.Close()
	c := configuration{Listen: front.Addr().String(), Routes: map[string]route{
		"accounts.nintendo.com": {Address: backend.Addr().String()},
	}}
	if err := validate(c); err != nil {
		t.Fatal(err)
	}
	go serve(front, c)
	_, err = tls.DialWithDialer(&net.Dialer{Timeout: time.Second}, "tcp", front.Addr().String(),
		&tls.Config{ServerName: "127.0.0.1", MinVersion: tls.VersionTLS12})
	if err == nil {
		t.Fatal("connection without SNI must be rejected")
	}
	if err := backend.(*net.TCPListener).SetDeadline(time.Now().Add(250 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	conn, err := backend.Accept()
	if err == nil {
		conn.Close()
		t.Fatal("no-SNI request reached the backend")
	}
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("unexpected backend error: %v", err)
	}
}

func TestFragmentedClientHello(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()
	client.SetDeadline(time.Now().Add(time.Second))
	server.SetDeadline(time.Now().Add(time.Second))
	go func() {
		c := tls.Client(client, &tls.Config{ServerName: "account.roomcircuit.test", MinVersion: tls.VersionTLS12})
		c.Handshake()
		c.Close()
	}()
	name, wire, err := hello(server)
	if err != nil || name != "account.roomcircuit.test" {
		t.Fatalf("name=%s err=%v", name, err)
	}
	var fragmented bytes.Buffer
	for at := 0; at < len(wire); {
		n := int(binary.BigEndian.Uint16(wire[at+3 : at+5]))
		payload := wire[at+5 : at+5+n]
		for _, b := range payload {
			fragmented.Write([]byte{22, 3, 3, 0, 1, b})
		}
		at += 5 + n
	}
	got, retained, err := hello(bytes.NewReader(fragmented.Bytes()))
	if err != nil || got != name || !bytes.Equal(retained, fragmented.Bytes()) {
		t.Fatalf("fragmented hello failed: %s %v", got, err)
	}
}
