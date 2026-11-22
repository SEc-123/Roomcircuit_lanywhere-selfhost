package main

import (
	"bytes"
	"crypto/sha256"
	"os"
	"path/filepath"
	"testing"
)

func TestPrivateDeviceStateSurvivesReopen(t *testing.T) {
	dir := t.TempDir()
	path, key := filepath.Join(dir, "devices.enc"), filepath.Join(dir, "key")
	if err := os.WriteFile(key, []byte(randomHex(32)), 0o600); err != nil {
		t.Fatal(err)
	}
	application := "0000000000001234"
	store, devices, err := openDeviceStore(path, key, application)
	if err != nil {
		t.Fatal(err)
	}
	id, password, grant := randomHex(16), randomHex(32), randomHex(64)
	want := device{password: sha256.Sum256([]byte(password)), grant: grant,
		owner: player{PID: 75432, Account: "private-account", User: "private-user", Application: application}}
	adapter := &adapter{store: store, devices: devices}
	if err := adapter.saveDevice(id, want); err != nil {
		t.Fatal(err)
	}
	if _, _, err := openDeviceStore(path, key, application); err == nil {
		t.Fatal("second writer unexpectedly acquired the store")
	}
	if err := store.lock.Close(); err != nil {
		t.Fatal(err)
	}
	encoded, err := privateRead(path, storeLimit)
	if err != nil {
		t.Fatal(err)
	}
	for _, secret := range []string{password, grant, want.owner.Account, want.owner.User} {
		if bytes.Contains(encoded, []byte(secret)) {
			t.Fatal("device state exposed protected data")
		}
	}
	if _, _, err := openDeviceStore(path, key, "000000000000ABCD"); err == nil {
		t.Fatal("device state opened in a different application")
	}
	reopened, restored, err := openDeviceStore(path, key, application)
	if err != nil {
		t.Fatal(err)
	}
	if got := restored[id]; got.password != want.password || got.owner.PID != want.owner.PID || got.owner.Account != want.owner.Account || got.owner.User != want.owner.User || got.grant != grant {
		t.Fatal("reopened device identity or proof differed")
	}
	if err := reopened.lock.Close(); err != nil {
		t.Fatal(err)
	}
	encoded[len(encoded)-1] ^= 0x80
	if err := os.WriteFile(path, encoded, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := openDeviceStore(path, key, application); err == nil {
		t.Fatal("modified state should fail authentication")
	}
}

func TestHTTPSOriginIsIndependentOfListener(t *testing.T) {
	for _, url := range []string{"https://roomcircuit.example.test", "https://192.0.2.10", "https://127.0.0.1:39282"} {
		if _, ok := origin(url, "https"); !ok {
			t.Fatalf("valid issuer refused: %s", url)
		}
	}
	for _, url := range []string{"http://roomcircuit.example.test", "https://user@roomcircuit.example.test", "https://roomcircuit.example.test/path"} {
		if _, ok := origin(url, "https"); ok {
			t.Fatalf("invalid issuer accepted: %s", url)
		}
	}
}
