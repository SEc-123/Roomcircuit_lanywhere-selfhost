# Username-only account core

This independent realm accepts only a username and password. It does not collect, verify, fabricate or send email, country or birth date. Usernames are case-insensitive ASCII, begin with a letter and contain 3–24 letters, digits or underscores. Passwords are 8–72 bytes and are stored as bcrypt cost 12 hashes. Account status, sessions, links, rate windows and unlink events are persisted in PostgreSQL tables prefixed `rc_user_`. A matching configuration is required on the broker, device identity bridge and account adapter.

A password cannot be recovered by email. Keep it safe. An account that is disabled cannot authenticate; unlink events invalidate old device grants. Existing email-based realms remain separately configured.

`go test -race ./...` checks host-level behavior. To exercise the complete disposable stack with PostgreSQL and actual core, identity, broker and adapter processes:

```sh
cd experimental/native-online/native-backend
python3 -B username-core/e2e.py
```

Docker, PostgreSQL 16, Go 1.26, and OpenSSL are required. The test creates a disposable loopback-only PostgreSQL container and temporary keys, then removes them. It verifies registration, duplicate rejection, login, OAuth approval, empty-email profile, account binding, private game identity, core restart, disabled account, and unlink/relink old-grant rejection. It uses a synthetic protocol client and an internal test key for loopback registration; real console and public routing are not tested. Ordinary `go test` skips the PostgreSQL lifecycle test unless `RC_USERNAME_TEST_DATABASE_URL` points to a disposable database.

The core listens on loopback and rejects missing internal keys. Broker and adapter edge limits use the socket peer, ignoring untrusted forwarding headers. A reverse proxy would aggregate clients under its own address unless a separately verified trusted proxy design is deployed. The current setup is a development fixture and is not ready for public exposure.
