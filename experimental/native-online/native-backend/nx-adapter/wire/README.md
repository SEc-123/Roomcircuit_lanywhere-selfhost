# Native account wire responses

These pure builders map existing RoomCircuit account data to BAAS and account profile response shapes. The caller verifies identity, membership, ownership, terms and client scope before constructing a response. The caller supplies the HTTPS service origins, current tokens and committed profile timestamps. The package has no HTTP clients, storage, signing keys or outbound routes.

The registration builder exposes the newly issued password only for its selected device. Ordinary login and profile responses omit passwords. Avatar uploads and subsequent reads use the same owned media URL. The caller commits profile and avatar changes before returning the corresponding response. Missing optional data stays absent or uses the protocol's empty value.

`testdata/accepted-login-shape.json` is the original value-stripped login sample from [OpenPak nx-baas at cf93fd71](https://github.com/openpak/nx-baas/tree/cf93fd71d05296ac321a90fa880af707bf35ed44). `testdata/na-profile-shape.json` contains type markers transcribed from the reference `/2.0.0/users/me` builder and omits unsupported external links. The second file is a source-derived contract. The accompanying AGPL-3.0 license applies to this adaptation and the copied fixture.

Run `go test ./wire` from the adapter directory. These tests cover response contracts and scope boundaries. Native HOS client acceptance needs separate device tests.
