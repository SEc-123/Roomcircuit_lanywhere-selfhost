package main

import (
	"crypto/sha256"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestGrantCommitKeepsUsableReplacementAfterDirectorySyncError(t *testing.T) {
	path := filepath.Join(t.TempDir(), "grants.json")
	store, err := openDurableGrants(path)
	if err != nil {
		t.Fatal(err)
	}
	id := strings.Repeat("a", 48)
	item := approvedDevice{Account: "synthetic", Subject: strings.Repeat("b", 64), PID: 5, RefreshUntil: time.Now().Add(time.Hour),
		RefreshSHA256: sha256.Sum256([]byte("original"))}
	if !store.commit(map[string]approvedDevice{id: item}) {
		t.Fatal("initial save failed")
	}
	item.RefreshSHA256 = sha256.Sum256([]byte("replacement"))
	store.syncDirectory = func(*os.File) error { return errors.New("synthetic post-rename fsync failure") }
	if !store.commit(map[string]approvedDevice{id: item}) || store.active[id] != item {
		t.Fatal("committed refresh rotation would withhold the replacement")
	}
	store.lock.Close()
	loaded, err := openDurableGrants(path)
	if err != nil {
		t.Fatal(err)
	}
	defer loaded.lock.Close()
	if loaded.active[id].RefreshSHA256 != item.RefreshSHA256 {
		t.Fatal("visible file still contains the consumed credential")
	}
	loaded.path = filepath.Join(t.TempDir(), "missing", "grants.json")
	if loaded.commit(map[string]approvedDevice{}) || loaded.active[id].RefreshSHA256 != item.RefreshSHA256 {
		t.Fatal("pre-rename failure changed the active grant")
	}
}
