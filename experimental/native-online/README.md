# Native Internet-mode experiment

**Draft, not usable on retail consoles.** This work explores an independent account and game service for the existing RoomCircuit project. It is separate from the current Local Wireless service.

The backend includes independent account linking, authorization and selected game-protocol profiles. Client-side experiments target HOS 19.0.1 and Atmosphère 1.12.0 in emuMMC. Actual console account linking, native game connection and multiplayer gameplay remain unverified. The code does not establish support for all games.

**Development configuration:** Builds require an explicit independent CA and server address. Preparing credentials, checking TLS, and compiling the candidate do not activate DNS or boot profiles. Server routes, device enrollment and physical-console acceptance are still required; this is not an installation package.

The account and game services are development fixtures; they are not connected to the public RoomCircuit release. Source and licenses are retained in this directory. No runtime account data, private keys or device reports are included.

- [Account and game service](native-backend/README.md)
- [Native account adapter](native-backend/nx-adapter/README.md)
- [Game protocol runtime](backend-lab/runtime/README.md)
- [Client experiments](native-client/README.md)
- [HOS 19 protocol notes](native-validation/backend/hos19-account-contract.md)
