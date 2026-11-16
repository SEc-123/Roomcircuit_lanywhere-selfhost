# Local test endpoint credentials

`deploy.sh` now prepares local files only; it never starts a listener or changes a live server. Supply an explicitly owned CA certificate and matching private key, an IPv4 address, and exact supported aliases. The output directory must not already exist.

```sh
sh deploy.sh --ca /private/ca.pem --ca-key /private/ca.key --address <server-ipv4> \
  --name accounts.nintendo.com --name dauth-lp1.ndas.srv.nintendo.net --output /private/endpoint-assets
```

The output includes a DER copy of the CA, the leaf certificate and its owner-only private key. It does not include the CA private key. Use the same DER file when building the network probe and trust-preparation NRO. It remains the operator's responsibility to configure the server, safely install its leaf key, verify all matching SNI routes, and prove real emuMMC isolation before any profile is activated.
