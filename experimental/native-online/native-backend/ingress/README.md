# Independent native TLS ingress

This experimental router preserves TLS end to end. It selects an explicitly configured loopback service from a bounded ClientHello SNI. It neither decrypts user credentials nor forwards a client-certificate header. The selected account, device or game service verifies its own TLS peer and application credentials. Unknown SNI is closed; no-SNI is also closed unless explicitly configured.

The optional `proxy_v1` prefix is for a game authentication listener configured to trust this one local ingress. Secure game listeners remain direct. Do not set that option for an ordinary TLS backend.

`go test -race ./...` uses real HTTPS/TLS handshakes, including a separate mTLS service that requires possession of an own client key. It checks missing client proof, unknown SNI, fragmented ClientHello records and malformed/nonlocal configuration. The router is not a certificate authority or a source of game authorization. Connections are capped at 128 and initial parsing is time/size bounded.

No public SDK alias profile is activated by this source. A reachable account profile still needs its matching own certificates, enrolled device proof, exact OAuth callback and emuMMC runtime acceptance.
