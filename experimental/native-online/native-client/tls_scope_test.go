package nativeclient

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"math/big"
	"net"
	"strings"
	"testing"
	"time"
)

// This is a host-side TLS check. It does not model the Switch trust store.
func TestIndependentIssuerScope(t *testing.T) {
	const namespace = "roomcircuit.invalid"
	const endpoint = "game." + namespace
	rootKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	root := certificateTemplate(1)
	root.IsCA = true
	root.BasicConstraintsValid = true
	root.KeyUsage = x509.KeyUsageCertSign
	root.PermittedDNSDomainsCritical = true
	root.PermittedDNSDomains = []string{namespace}
	rootDER, err := x509.CreateCertificate(rand.Reader, root, root, &rootKey.PublicKey, rootKey)
	if err != nil {
		t.Fatal(err)
	}
	root, err = x509.ParseCertificate(rootDER)
	if err != nil {
		t.Fatal(err)
	}
	roots := x509.NewCertPool()
	roots.AddCert(root)

	for _, tc := range []struct {
		name       string
		leafName   string
		serverName string
		roots      *x509.CertPool
		wantError  string
	}{
		{"own issuer and hostname", endpoint, endpoint, roots, ""},
		{"unexpected hostname", endpoint, "other." + namespace, roots, "certificate is valid for"},
		{"untrusted issuer", endpoint, endpoint, x509.NewCertPool(), "certificate signed by unknown authority"},
		{"issuer outside allowed namespace", "outside.invalid", "outside.invalid", roots, "not permitted by any constraint"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			leafKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
			if err != nil {
				t.Fatal(err)
			}
			leaf := certificateTemplate(2)
			leaf.DNSNames = []string{tc.leafName}
			leaf.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
			leafDER, err := x509.CreateCertificate(rand.Reader, leaf, root, &leafKey.PublicKey, rootKey)
			if err != nil {
				t.Fatal(err)
			}
			server := tls.Certificate{Certificate: [][]byte{leafDER}, PrivateKey: leafKey}
			err = handshake(server, tc.roots, tc.serverName)
			if tc.wantError == "" {
				if err != nil {
					t.Fatal(err)
				}
			} else if err == nil || !strings.Contains(err.Error(), tc.wantError) {
				t.Fatalf("handshake error = %v; want %q", err, tc.wantError)
			}
		})
	}
}

func certificateTemplate(serial int64) *x509.Certificate {
	now := time.Now()
	return &x509.Certificate{
		SerialNumber: big.NewInt(serial),
		NotBefore:    now.Add(-time.Hour),
		NotAfter:     now.Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
	}
}

func handshake(cert tls.Certificate, roots *x509.CertPool, name string) error {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()
	deadline := time.Now().Add(5 * time.Second)
	if err := client.SetDeadline(deadline); err != nil {
		return err
	}
	if err := server.SetDeadline(deadline); err != nil {
		return err
	}
	serverResult := make(chan error, 1)
	go func() {
		serverResult <- tls.Server(server, &tls.Config{
			Certificates: []tls.Certificate{cert},
			MinVersion:   tls.VersionTLS12,
		}).Handshake()
	}()
	err := tls.Client(client, &tls.Config{
		RootCAs:    roots,
		ServerName: name,
		MinVersion: tls.VersionTLS12,
	}).Handshake()
	if err != nil {
		client.Close()
		<-serverResult
		return err
	}
	return <-serverResult
}
