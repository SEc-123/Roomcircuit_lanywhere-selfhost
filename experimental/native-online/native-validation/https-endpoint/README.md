# Local test endpoint credentials

For an isolated test with no real device identity, `generate_test_ca.py` creates a short-lived private test CA. Its critical DNS name constraints cover only explicitly selected SDK aliases and their subdomains. Keep its CA key private. `prepare_endpoint.py` issues a leaf with the exact selected aliases and IPv4 in its SAN. `create_local_profile.py` validates the CA, key, chain and SAN and writes loopback-only adapter and ingress templates. The adapter template contains an empty enrollment list and deliberately fails closed until a device is enrolled; it does not run anything.

`python3 test_local_profile.py` starts the actual adapter and ingress on temporary loopback ports using only synthetic certificates. It verifies the seven HOS 19 account aliases, rejects unsupported game and newer-system aliases, checks that unregistered devices cannot get a DAUTH challenge, and verifies that the username applet does not require a client certificate while DAUTH requests one. No HTTP request is sent to an outside host. Username registration, SDK game listeners, real console enrollment and public ingress are not supplied by the template.

`deploy.sh` now prepares local files only; it never starts a listener or changes a live server. Supply an explicitly owned CA certificate and matching private key, an IPv4 address, and exact supported aliases. The output directory must not already exist.

```sh
sh deploy.sh --ca /private/ca.pem --ca-key /private/ca.key --address <server-ipv4> \
  --name accounts.nintendo.com --name dauth-lp1.ndas.srv.nintendo.net --output /private/endpoint-assets
```

The output includes a DER copy of the CA, the leaf certificate and its owner-only private key. It does not include the CA private key. Use the same DER file when building the network probe and trust-preparation NRO. It remains the operator's responsibility to configure the server, safely install its leaf key, verify all matching SNI routes, and prove real emuMMC isolation before any profile is activated.
