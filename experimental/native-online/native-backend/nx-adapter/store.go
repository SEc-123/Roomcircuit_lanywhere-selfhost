package main

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"maps"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const storeLimit = 2 << 20
const storeMagic = "RCNX1\x00"

type storedDevice struct {
	PasswordSHA256 string `json:"password_sha256"`
	PID            uint64 `json:"pid"`
	User           string `json:"user"`
	Account        string `json:"account"`
	Grant          string `json:"grant,omitempty"`
	Enrollment     string `json:"enrollment,omitempty"`
	Provisional    string `json:"provisional,omitempty"`
	RegisteredAt   int64  `json:"registered_at,omitempty"`
	CreatedAt      int64  `json:"created_at,omitempty"`
	LinkedAt       int64  `json:"linked_at,omitempty"`
}

type deviceStore struct {
	path        string
	application string
	cipher      cipher.AEAD
	lock        *os.File
}

func privateRead(path string, limit int64) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0o077 != 0 || info.Size() > limit {
		return nil, errors.New("device store and key require bounded owner-only files")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return io.ReadAll(io.LimitReader(file, limit+1))
}

func openDeviceStore(path, keyPath, application string) (*deviceStore, map[string]device, error) {
	if path == "" || keyPath == "" {
		return nil, nil, errors.New("device store path and encryption key are required")
	}
	text, err := privateRead(keyPath, 96)
	if err != nil {
		return nil, nil, err
	}
	key, err := hex.DecodeString(strings.TrimSpace(string(text)))
	if err != nil || len(key) != 32 {
		return nil, nil, errors.New("device store key must contain 32 hexadecimal-encoded bytes")
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, nil, err
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, nil, err
	}
	lock, err := os.OpenFile(path+".lock", os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, nil, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		lock.Close()
		return nil, nil, fmt.Errorf("device store already in use: %w", err)
	}
	store := &deviceStore{path: path, application: application, cipher: aead, lock: lock}
	devices, err := store.load()
	if err != nil {
		lock.Close()
		return nil, nil, err
	}
	return store, devices, nil
}

func (s *deviceStore) associatedData() []byte {
	return []byte(storeMagic + s.application)
}

func (s *deviceStore) load() (map[string]device, error) {
	encoded, err := privateRead(s.path, storeLimit)
	if errors.Is(err, os.ErrNotExist) {
		return make(map[string]device), nil
	}
	if err != nil {
		return nil, err
	}
	start := len(storeMagic)
	endNonce := start + s.cipher.NonceSize()
	if len(encoded) < endNonce+s.cipher.Overhead() || !bytes.HasPrefix(encoded, []byte(storeMagic)) {
		return nil, errors.New("device store has an invalid envelope")
	}
	plain, err := s.cipher.Open(nil, encoded[start:endNonce], encoded[endNonce:], s.associatedData())
	if err != nil {
		return nil, errors.New("device store authentication failed")
	}
	var records map[string]storedDevice
	decoder := json.NewDecoder(bytes.NewReader(plain))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&records) != nil || !errors.Is(decoder.Decode(new(any)), io.EOF) || records == nil || len(records) > deviceLimit {
		return nil, errors.New("device store has invalid records")
	}
	devices := make(map[string]device, len(records))
	for id, record := range records {
		idBytes, idErr := hex.DecodeString(id)
		password, hashErr := hex.DecodeString(record.PasswordSHA256)
		enrollment, enrollErr := hex.DecodeString(record.Enrollment)
		legacy := len(enrollment) == 0 && len(idBytes) == 16
		native := len(enrollment) == 32 && deviceKey(enrollment) != (deviceKey{}) && len(idBytes) == 8
		provisional, provisionalErr := hex.DecodeString(record.Provisional)
		unlinked := native && provisionalErr == nil && len(provisional) == 8 && record.RegisteredAt > 0 &&
			record.PID == 0 && record.User == "" && record.Account == "" && record.Grant == ""
		linked := record.Provisional == "" && record.RegisteredAt == 0 &&
			record.PID != 0 && record.User != "" && record.Account != ""
		if idErr != nil || id != strings.ToLower(id) || hashErr != nil || len(password) != 32 ||
			enrollErr != nil || (!legacy && !native) || (!linked && !unlinked) ||
			(native && linked && record.Grant == "") || len(record.Grant) > 2048 || record.CreatedAt < 0 || record.LinkedAt < 0 ||
			(record.LinkedAt > 0 && (record.LinkedAt < record.CreatedAt || unlinked)) {
			return nil, errors.New("device store has an invalid device record")
		}
		item := device{grant: record.Grant, provisional: record.Provisional, registeredAt: record.RegisteredAt,
			createdAt: record.CreatedAt, linkedAt: record.LinkedAt}
		if linked {
			item.owner = player{PID: record.PID, User: record.User, Account: record.Account, Application: s.application}
		}
		copy(item.enrollment[:], enrollment)
		copy(item.password[:], password)
		devices[id] = item
	}
	return devices, nil
}

// The rename is the commit point; the caller follows the committed state even
// if the directory sync reports that crash durability cannot be guaranteed.
func (s *deviceStore) write(devices map[string]device) (committed bool, err error) {
	records := make(map[string]storedDevice, len(devices))
	for id, item := range devices {
		var enrollment string
		if item.enrollment != (deviceKey{}) {
			enrollment = hex.EncodeToString(item.enrollment[:])
		}
		records[id] = storedDevice{PasswordSHA256: hex.EncodeToString(item.password[:]), PID: item.owner.PID,
			User: item.owner.User, Account: item.owner.Account, Grant: item.grant, Enrollment: enrollment,
			Provisional: item.provisional, RegisteredAt: item.registeredAt, CreatedAt: item.createdAt, LinkedAt: item.linkedAt}
	}
	plain, err := json.Marshal(records)
	if err != nil {
		return false, err
	}
	nonce := make([]byte, s.cipher.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return false, err
	}
	data := s.cipher.Seal(append([]byte(storeMagic), nonce...), nonce, plain, s.associatedData())
	dir, err := os.Open(filepath.Dir(s.path))
	if err != nil {
		return false, err
	}
	defer dir.Close()
	file, err := os.CreateTemp(filepath.Dir(s.path), ".device-state-")
	if err != nil {
		return false, err
	}
	defer os.Remove(file.Name())
	defer file.Close()
	if _, err := file.Write(data); err != nil {
		return false, err
	}
	if err := file.Sync(); err != nil {
		return false, err
	}
	if err := file.Close(); err != nil {
		return false, err
	}
	if err := os.Rename(file.Name(), s.path); err != nil {
		return false, err
	}
	return true, dir.Sync()
}

// The adapter mutex is held by the caller.
func (a *adapter) saveDevice(id string, item device) error {
	next := maps.Clone(a.devices)
	now := time.Now()
	for key, existing := range next {
		if existing.registrationExpired(now) {
			delete(next, key)
		}
	}
	next[id] = item
	committed, err := a.store.write(next)
	if committed {
		a.devices = next
		if err != nil {
			log.Printf("device state committed; directory sync failed: %v", err)
		}
		return nil
	}
	return err
}
