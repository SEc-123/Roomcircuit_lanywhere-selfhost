# HOS 19.0.1 private trust preparation

**Development only:** The embedded test CA is environment-specific and expires. This component is not a production installation package.

The NRO checks emuMMC before filesystem startup, then requires HOS 19.0.1 and the exact certificate resource hash captured from the active emuMMC. It reads that resource through the active virtual system and writes only SD files. It never opens physical NAND storage or modifies a user account.

Press A to stage a private one-file RomFS and its SHA-256 sidecar under `sd:/atmosphere/roomcircuit-native/CertStore/`. Existing different files are preserved. The standard boot entry and original Atmosphere package are unchanged. The custom network build alone reads this private location, and only for the SSL process in emuMMC. The sidecar detects accidental corruption; it is not a signature or an authorization boundary.

Only expired certificate slot 1033 changes. All original certificate payload bytes and other entries remain in the private file. The test CA is limited to the independent test endpoint; the native service still verifies the certificate, hostname and time. This staging operation does not prove that a retail account or game requests that certificate.

`python3 test_store.py` checks parser failures and source preservation, then extracts the generated image with hactool. The extraction test requires Docker and a running container named `switch-nodes-sdk-080` with `/opt/devkitpro/tools/bin/hactool` installed. CI does not run this Docker-dependent test. Physical preparation and custom boot acceptance remain pending.
