# Independent endpoint TLS preflight

This checks the certificate chain and SNI hostname against a supplied CA and explicit server IPv4. It sends no account or device credentials, performs no HTTP request, and changes no Switch files. Use only an endpoint you control.

```sh
python3 check.py --address <server-ipv4> --ca /path/to/own-ca.pem \
  --name accounts.nintendo.com --name dauth-lp1.ndas.srv.nintendo.net
```

A passing handshake establishes TLS reachability for that alias only. It does not validate the account or game routes, device certificate enrollment, HOS system trust, or gameplay. An EOF, timeout, certificate error, or a rejected mutual-TLS handshake is a failure or unresolved condition, not proof of its root cause. No DNS profile or boot entry is generated or activated.
