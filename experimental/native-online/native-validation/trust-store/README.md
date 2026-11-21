# HOS 19.0.1 private trust preparation

Build with an explicitly supplied own DER CA. The same certificate must be used to issue the corresponding server certificate. The build validates the certificate and creates the embedded copy and matching SHA-256 header together; a missing, invalid or expired CA stops the build.

```sh
make CA_DER=/path/to/owned/ca.der
```

On a confirmed HOS 19.0.1 emuMMC only, this experimental NRO can read the active certificate store and stage a private one-file RomFS plus digest under `sd:/atmosphere/roomcircuit-native/CertStore/`. It does not modify the ordinary boot entry or activate the candidate. Existing different files are preserved. The digest detects corruption, not malicious replacement. The custom SSL-only emuMMC module must still be validated separately. This does not prove that a system account or game trusts the server.

`python3 test_store.py` checks parsing and independently extracts RomFS using a running Docker container named `switch-nodes-sdk-080` with hactool. CI runs the independent C/C++ format tests but not this container-dependent extraction. No hardware boot or gameplay acceptance is claimed.
