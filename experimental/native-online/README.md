# Native Internet-mode experiment

**Draft, not usable on retail consoles.** This work explores an independent account and game service for the existing RoomCircuit project. It is separate from the current Local Wireless service.

The backend includes independent account linking, authorization and selected game-protocol profiles. Client-side experiments target HOS 19.0.1 and Atmosphère 1.12.0 in emuMMC. Actual console account linking, native game connection and multiplayer gameplay remain unverified. The code does not establish support for all games.

**Development configuration:** Some experimental DNS rules and probe sources contain a fixed test-server IP and an embedded test CA. Their certificates are time-limited. These settings are tied to one test environment and must not be used as a deployment profile for other users or production. Configure an independently owned endpoint and trust chain before further testing; do not activate the candidate boot or DNS profile as an installation step.

The account and game services are development fixtures; they are not connected to the public RoomCircuit release. Source and licenses are retained in this directory. No runtime account data, private keys or device reports are included.

- [Account and game service](native-backend/README.md)
- [Native account adapter](native-backend/nx-adapter/README.md)
- [Game protocol runtime](backend-lab/runtime/README.md)
- [Client experiments](native-client/README.md)
- [HOS 19 protocol notes](native-validation/backend/hos19-account-contract.md)
