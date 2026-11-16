# Native network component for the current emuMMC

Target system `19.0.1 | AMS 1.12.0 | E`.

The build uses [Atmosphère 1.12.0](https://github.com/Atmosphere-NX/Atmosphere/tree/28d6a2e11f2640077bbb8a9fbd463d3fa12b6984). The [emuMMC-scoped variant](emummc-network-only.patch) checks the reported boot mode before the HOME-menu fallback. The [private CertStore variant](private-certstore.patch) directs only the SSL process's built-in CertStore request to the private resource, with a second emuMMC check before opening files. Its build target is `stratosphere/ams_mitm` in isolated Docker storage. TLS validation remains in the system SSL service.

The private resource is `atmosphere/roomcircuit-native/CertStore/romfs.bin`. Its adjacent `romfs.sha256` file holds 64 lowercase hexadecimal characters and a newline. The loader checks the one-file RomFS layout, the BDF record bounds and the digest, then serves an immutable copy capped at 256 KiB per open. The sidecar detects inconsistent bytes within the SD trust boundary. Missing or invalid input forwards to the built-in storage. The component makes a single read attempt, with latency governed by the SD filesystem.

The local package3 candidate uses the independently matched official AMS 1.12.0 baseline. The package verification confirmed fourteen unchanged component payloads and offsets. The candidate and receipt are in the local `.artifacts` directory. Physical boot and gameplay acceptance are pending.

## Independent TLS checks

```bash
go test -v native-client/tls_scope_test.go
```

The host fixture accepts its own issuer and expected hostname. It rejects a wrong hostname, an untrusted issuer and a certificate outside the issuer's allowed DNS namespace. The fixture uses synthetic local certificates and exercises the Go TLS implementation.

```bash
python3 native-client/test_emummc_scope.py --private-certstore /path/to/atmosphere-1.12.0
```

The test applies the patch to a temporary copy and executes the actual selection predicates with a mocked SPL configuration response. It covers active emuMMC, sysMMC, a failed read and an unknown value before and after HOME starts.

The format test accepts a fixture from the native trust-store builder and rejects malformed metadata, truncated images, out-of-range BDF records and malformed digest sidecars.

## Private DNS profile

The local DNS candidate adds a read-only `atmosphere/roomcircuit-native/DNS/profile.txt` input. The loader requires HOS 19.0.1 and reads the file only after confirming emuMMC type 1. The profile selects exact aliases from the pinned account and two game contracts. The profile must explicitly specify the target IPv4 address; there is no compiled-in endpoint. Selected aliases point to that address. Missing, unreadable or invalid input leaves the existing DNS rules in effect. The private IPv4 replies reject unsupported address families and malformed hints through the normal resolver error fields.

The candidate and package readback receipt are stored separately in `.artifacts`. An active profile requires matching independent TLS certificates and working local service routes for every selected alias. The actual server certificate and SNI routes must cover each selected alias; a test using a separately imported CA is not proof of the system account trust path. Physical acceptance for the account and game aliases remains pending.

```bash
python3 -B native-client/test_private_dns_scope.py /path/to/atmosphere-1.12.0
c++ -std=c++20 -Wall -Wextra -Werror native-client/test_private_dns.cpp -o /tmp/roomcircuit-test-private-dns
/tmp/roomcircuit-test-private-dns
```

The profile format starts with `RoomCircuitNativeDNS1 <IPv4>` and a newline, followed by one exact supported hostname per line. Unknown names, duplicate names, malformed addresses and empty profiles are rejected. The endpoint is never inferred and no profile is installed or activated by the build. Use the TLS-only preflight in `native-validation/preflight/` before preparing any real configuration.

## Native acceptance

The read-only device probe found a 63-entry system trust resource on HOS 19.0.1. OpenPak documents a 127-entry serialized resource for HOS 22.5.0. The independent service needs a verified trust resource derived for the current system and a signed network profile using its own issuer. The first physical acceptance run must confirm TLS validation, binding to the independent account, entering the game's Internet menu, and two-player room and gameplay behavior.

[OpenPak trust-resource evidence](https://github.com/openpak/switch-patcher/blob/v0.3.34/source/system.c#L277-L293) · [Independent network requirements](https://docs.openpak.org/self-hosting/)
