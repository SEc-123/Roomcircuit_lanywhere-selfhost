package main

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"roomcircuit.local/nx-adapter/wire"
)

const nativeProfileMax = 512 << 10

type nativeProfile struct {
	CreatedAt int64            `json:"created_at"`
	UpdatedAt int64            `json:"updated_at"`
	Profile   wire.BaaSProfile `json:"profile"`
	Country   string           `json:"country,omitempty"`
	Birthday  string           `json:"birthday,omitempty"`
	Avatar    []byte           `json:"avatar,omitempty"`
}

type nativeProfileStore struct {
	mu        sync.Mutex
	dir       string
	container *deviceStore
}

func newNativeProfileStore(container *deviceStore) (*nativeProfileStore, error) {
	if container == nil {
		return nil, errors.New("native profiles require a device store")
	}
	dir := container.path + ".profiles"
	if err := os.Mkdir(dir, 0o700); err != nil && !errors.Is(err, os.ErrExist) {
		return nil, err
	}
	info, err := os.Lstat(dir)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() || info.Mode().Perm()&0o077 != 0 {
		return nil, errors.New("native profile directory must be owner-only")
	}
	return &nativeProfileStore{dir: dir, container: container}, nil
}

func validNativeUserID(id string) bool {
	decoded, err := hex.DecodeString(id)
	return err == nil && len(decoded) == 8 && id == strings.ToLower(id)
}

func (s *nativeProfileStore) associatedData(id string) []byte {
	return []byte("roomcircuit-baas-profile-v1\x00" + s.container.application + "\x00" + id)
}

func (s *nativeProfileStore) read(id string) (nativeProfile, error) {
	var profile nativeProfile
	if !validNativeUserID(id) {
		return profile, errors.New("invalid native profile identifier")
	}
	data, err := privateRead(filepath.Join(s.dir, id), nativeProfileMax)
	if errors.Is(err, os.ErrNotExist) {
		return profile, nil
	}
	if err != nil {
		return profile, err
	}
	aead := s.container.cipher
	n := aead.NonceSize()
	if len(data) < n+aead.Overhead() {
		return profile, errors.New("invalid native profile envelope")
	}
	plain, err := aead.Open(nil, data[:n], data[n:], s.associatedData(id))
	if err != nil {
		return profile, errors.New("native profile authentication failed")
	}
	decoder := json.NewDecoder(bytes.NewReader(plain))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&profile); err != nil || decoder.Decode(new(any)) != io.EOF || len(profile.Avatar) > nativeAvatarMax {
		return nativeProfile{}, errors.New("invalid native profile data")
	}
	return profile, nil
}

// The caller holds s.mu across each read-modify-write.
func (s *nativeProfileStore) write(id string, profile nativeProfile) error {
	if !validNativeUserID(id) || len(profile.Avatar) > nativeAvatarMax {
		return errors.New("invalid native profile record")
	}
	plain, err := json.Marshal(profile)
	if err != nil {
		return err
	}
	aead := s.container.cipher
	nonce := make([]byte, aead.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return err
	}
	data := aead.Seal(nonce, nonce, plain, s.associatedData(id))
	if len(data) > nativeProfileMax {
		return errors.New("native profile exceeds storage limit")
	}
	file, err := os.CreateTemp(s.dir, ".native-profile-")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	defer file.Close()
	if _, err := file.Write(data); err != nil {
		return err
	}
	if err := file.Sync(); err != nil {
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	dir, err := os.Open(s.dir)
	if err != nil {
		return err
	}
	defer dir.Close()
	if err := os.Rename(file.Name(), filepath.Join(s.dir, id)); err != nil {
		return err
	}
	return dir.Sync()
}
