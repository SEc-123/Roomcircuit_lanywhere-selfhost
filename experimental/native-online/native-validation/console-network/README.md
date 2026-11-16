# RoomCircuit Network Probe

The probe connects to an explicitly configured server IPv4 address using an explicitly provided independent CA. It checks the HOS SSL service on HOS 19.0.1 in emuMMC type 1. It makes fresh TLS contexts for the expected host, a wrong hostname, and an absent CA. It also queries whether the active virtual SSL service can register its internal client identity, without sending or exporting that identity.

Build both the probe and trust-preparation tool with the **same** own CA DER created by `../https-endpoint/prepare_endpoint.py`. Set `PROBE_IPV4` to the endpoint used to create its server certificate. A missing, invalid or expired CA or an invalid address stops the build. These are local build steps and do not modify the console or server.

```sh
make CA_DER=/path/to/owned/ca.der PROBE_IPV4=<server-ipv4>
```

The only persistent probe output is `sd:/switch/roomcircuit-native/network-probe.txt`. Temporary context trust does not prove that the system account client trusts the service. The current public listener still needs separately verified alias certificates and working routes. Do not activate a DNS profile or candidate boot until those prerequisites and emuMMC isolation are established.
