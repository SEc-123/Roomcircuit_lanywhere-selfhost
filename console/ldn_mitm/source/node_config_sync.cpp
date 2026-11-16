// SPDX-License-Identifier: GPL-2.0-only
#include "node_config_sync.hpp"
#include "storage_migration.hpp"
#include "../../../common/roomcircuit_paths.h"
#include "../../../common/game_identity_sha256.h"
#include <stratosphere.hpp>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace ams::mitm::storage {
namespace {
constexpr size_t MaxConfig = 8192;
constexpr const char *Old = RC_V1_CONFIG_DIR "/nodes.ini";
constexpr const char *Current = RC_CONFIG_DIR "/nodes.ini";
constexpr const char *Work = RC_CONFIG_DIR "/.nodes-sync";
constexpr const char *Owner = RC_CONFIG_DIR "/.nodes-sync/owner";
constexpr const char *Temp = RC_CONFIG_DIR "/.nodes-sync/nodes.tmp";
constexpr const char *Backup = RC_CONFIG_DIR "/.nodes-sync/nodes.previous";
constexpr const char *Journal = RC_CONFIG_DIR "/.nodes-sync/journal";
constexpr const char *JournalTemp = RC_CONFIG_DIR "/.nodes-sync/journal.tmp";
constexpr const char *State = RC_CONFIG_DIR "/.nodes-sync/state";
constexpr const char *StateTemp = RC_CONFIG_DIR "/.nodes-sync/state.tmp";
constexpr const char *StateBackup = RC_CONFIG_DIR "/.nodes-sync/state.previous";

struct File { bool exists; size_t size; u8 bytes[MaxConfig]; u8 hash[32]; };
struct Record { u8 magic[4]; u8 flags; u8 reserved[3]; u8 old_hash[32], current_hash[32], checksum[32]; };
struct Scratch { File old, current, temp, backup; Record state, journal; };

void Hash(const void *data, size_t size, u8 out[32]) {
    GameIdentitySha256 digest; gi_sha256_init(&digest); gi_sha256_update(&digest, data, size); gi_sha256_final(&digest, out);
}
bool Same(const u8 a[32], const u8 b[32]) { return std::memcmp(a, b, 32) == 0; }
bool FileHas(const File &f, const u8 hash[32]) { return f.exists && Same(f.hash, hash); }
bool Read(const char *path, File *out) {
    *out = {};
    fs::FileHandle file;
    const Result opened = fs::OpenFile(&file, path, fs::OpenMode_Read);
    if (fs::ResultPathNotFound::Includes(opened)) return true;
    if (R_FAILED(opened)) return false;
    s64 size = -1; size_t actual = 0;
    const bool ok = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size >= 0 &&
        static_cast<u64>(size) <= MaxConfig &&
        R_SUCCEEDED(fs::ReadFile(&actual, file, 0, out->bytes, static_cast<size_t>(size))) &&
        actual == static_cast<size_t>(size);
    fs::CloseFile(file);
    if (!ok) return false;
    out->exists = true; out->size = static_cast<size_t>(size); Hash(out->bytes, out->size, out->hash);
    return true;
}
bool ReadRecord(const char *path, const char magic[4], Record *out, bool *exists) {
    fs::FileHandle file;
    const Result opened = fs::OpenFile(&file, path, fs::OpenMode_Read);
    if (fs::ResultPathNotFound::Includes(opened)) { *exists = false; return true; }
    if (R_FAILED(opened)) return false;
    *exists = true;
    s64 size = -1; size_t actual = 0;
    const bool read = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size == static_cast<s64>(sizeof(*out)) &&
        R_SUCCEEDED(fs::ReadFile(&actual, file, 0, out, sizeof(*out))) && actual == sizeof(*out);
    fs::CloseFile(file);
    if (!read) return false;
    u8 checksum[32]; Hash(out, offsetof(Record, checksum), checksum);
    return !std::memcmp(out->magic, magic, 4) && !(out->flags & ~3u) &&
        !out->reserved[0] && !out->reserved[1] && !out->reserved[2] && Same(checksum, out->checksum);
}
bool Remove(const char *path) {
    const Result result = fs::DeleteFile(path);
    return R_SUCCEEDED(result) || fs::ResultPathNotFound::Includes(result);
}
bool WriteFresh(const char *path, const void *bytes, size_t size) {
    if (R_FAILED(fs::CreateFile(path, static_cast<s64>(size)))) return false;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, path, fs::OpenMode_Write))) return false;
    const bool ok = R_SUCCEEDED(fs::WriteFile(file, 0, bytes, size, fs::WriteOption::Flush));
    fs::CloseFile(file);
    return ok;
}
bool Rename(const char *from, const char *to) { return R_SUCCEEDED(fs::RenameFile(from, to)); }
bool EnsureOwnedWork() {
    bool exists = false;
    if (R_FAILED(fs::HasDirectory(&exists, Work))) return false;
    if (!exists && R_FAILED(fs::CreateDirectory(Work))) return false;
    bool owner = false;
    if (R_FAILED(fs::HasFile(&owner, Owner))) return false;
    if (owner) return true;
    fs::DirectoryHandle dir;
    if (R_FAILED(fs::OpenDirectory(&dir, Work, fs::OpenDirectoryMode_All))) return false;
    s64 count = -1; const Result count_result = fs::GetDirectoryEntryCount(&count, dir);
    fs::CloseDirectory(dir);
    if (R_FAILED(count_result) || count != 0) return false;
    return WriteFresh(Owner, "", 0);
}
bool StateRecovery() {
    Record current{}, backup{}; bool have = false, have_backup = false;
    if (!ReadRecord(State, "RCS2", &current, &have) || !ReadRecord(StateBackup, "RCS2", &backup, &have_backup)) return false;
    if (have_backup && !have && !Rename(StateBackup, State)) return false;
    if (have_backup && have && !Remove(StateBackup)) return false;
    if (!Remove(StateTemp) || !Remove(JournalTemp)) return false;
    return true;
}
bool SaveState(const File &old, const File &current) {
    if (!StateRecovery()) return false;
    Record next{}; std::memcpy(next.magic, "RCS2", 4);
    next.flags = (old.exists ? 1 : 0) | (current.exists ? 2 : 0);
    if (old.exists) std::memcpy(next.old_hash, old.hash, 32);
    if (current.exists) std::memcpy(next.current_hash, current.hash, 32);
    Hash(&next, offsetof(Record, checksum), next.checksum);
    Record prior{}; bool exists = false;
    if (!ReadRecord(State, "RCS2", &prior, &exists)) return false;
    if (exists && !std::memcmp(&prior, &next, sizeof(next))) return true;
    if (!WriteFresh(StateTemp, &next, sizeof(next))) return false;
    Record verify{}; bool have = false;
    if (!ReadRecord(StateTemp, "RCS2", &verify, &have) || !have || std::memcmp(&verify, &next, sizeof(next))) return false;
    if (exists && !Rename(State, StateBackup)) return false;
    if (!Rename(StateTemp, State)) return false;
    if (exists && !Remove(StateBackup)) return false;
    return true;
}
bool SaveJournal(const File &old, const File &current) {
    Record j{}; std::memcpy(j.magic, "RCJ2", 4);
    j.flags = 1 | (current.exists ? 2 : 0);
    std::memcpy(j.old_hash, old.hash, 32);
    if (current.exists) std::memcpy(j.current_hash, current.hash, 32);
    Hash(&j, offsetof(Record, checksum), j.checksum);
    if (!WriteFresh(JournalTemp, &j, sizeof(j))) return false;
    Record verify{}; bool exists = false;
    return ReadRecord(JournalTemp, "RCJ2", &verify, &exists) && exists &&
        !std::memcmp(&j, &verify, sizeof(j)) && Rename(JournalTemp, Journal);
}
bool RecoverSync(Scratch &s) {
    bool journal_exists = false;
    if (!ReadRecord(Journal, "RCJ2", &s.journal, &journal_exists) ||
        !Read(Temp, &s.temp) || !Read(Backup, &s.backup)) return false;
    if (!journal_exists) return !s.backup.exists && (!s.temp.exists || Remove(Temp));
    const Record &j = s.journal;
    if (!(j.flags & 1) || !Read(Current, &s.current)) return false;
    if (s.backup.exists && (!(j.flags & 2) || !Same(s.backup.hash, j.current_hash))) {
        // Preserve an externally changed file which raced the move to our
        // backup, including when a read fault interrupted that move.
        if ((j.flags & 2) && !s.current.exists && Rename(Backup, Current)) {
            (void)Remove(Temp); (void)Remove(Journal);
        }
        return false;
    }
    if (FileHas(s.current, j.old_hash)) {
        // The new canonical file is already installed. Record the journal's
        // observed sources even if the legacy script changed again meanwhile.
        s.old = {}; s.old.exists = true; std::memcpy(s.old.hash, j.old_hash, 32);
        if (s.temp.exists && !Same(s.temp.hash, j.old_hash)) return false;
        if (!SaveState(s.old, s.current)) return false;
        if (s.backup.exists && !Remove(Backup)) return false;
    } else {
        if (s.current.exists && (!(j.flags & 2) || !Same(s.current.hash, j.current_hash))) return false;
        if (!s.current.exists && (j.flags & 2)) {
            if (!s.backup.exists || !Rename(Backup, Current)) return false;
        } else if (s.backup.exists) return false;
    }
    if (s.temp.exists && !Remove(Temp)) return false;
    return Remove(Journal);
}
bool InstallLegacy(Scratch &scratch) {
    const File &old = scratch.old, &current = scratch.current;
    // Refuse to overwrite external edits that raced the initial read.
    if (!Read(Old, &scratch.temp) || !Read(Current, &scratch.backup) ||
        !FileHas(scratch.temp, old.hash) || scratch.backup.exists != current.exists ||
        (current.exists && !Same(scratch.backup.hash, current.hash))) return false;
    if (!SaveJournal(old, current) || !WriteFresh(Temp, old.bytes, old.size)) return false;
    if (!Read(Temp, &scratch.temp) || !FileHas(scratch.temp, old.hash)) return false;
    if (!Read(Old, &scratch.temp) || !Read(Current, &scratch.backup) || !FileHas(scratch.temp, old.hash) ||
        scratch.backup.exists != current.exists || (current.exists && !Same(scratch.backup.hash, current.hash))) return false;
    if (current.exists) {
        if (!Rename(Current, Backup) || !Read(Backup, &scratch.backup)) return false;
        if (!FileHas(scratch.backup, current.hash)) {
            // An external writer raced the final check. Put its file back;
            // never discard it merely because it occupies our backup path.
            if (Rename(Backup, Current)) { (void)Remove(Temp); (void)Remove(Journal); }
            return false;
        }
    }
    if (!Rename(Temp, Current)) return false;
    if (!Read(Current, &scratch.temp) || !FileHas(scratch.temp, old.hash) || !SaveState(old, scratch.temp)) return false;
    if (current.exists && !Remove(Backup)) return false;
    return Remove(Journal);
}
}

bool ReadNodeConfig(char *out, size_t capacity, size_t *actual) {
    if (!out || !actual || capacity < MaxConfig) return false;
    *actual = 0;
    std::unique_ptr<Scratch, decltype(&std::free)> s(static_cast<Scratch *>(std::calloc(1, sizeof(Scratch))), &std::free);
    if (!s) return false;
    if (StorageGeneration() != 3) {
        if (!Read(Old, &s->old)) return false;
        if (s->old.exists) { std::memcpy(out, s->old.bytes, s->old.size); *actual = s->old.size; }
        return true;
    }
    if (!EnsureOwnedWork() || !StateRecovery() || !RecoverSync(*s) || !Read(Old, &s->old) || !Read(Current, &s->current)) return false;
    bool state_exists = false;
    if (!ReadRecord(State, "RCS2", &s->state, &state_exists)) return false;
    const bool old_changed = !state_exists || s->old.exists != bool(s->state.flags & 1) ||
        (s->old.exists && !Same(s->old.hash, s->state.old_hash));
    const bool current_changed = !state_exists || s->current.exists != bool(s->state.flags & 2) ||
        (s->current.exists && !Same(s->current.hash, s->state.current_hash));
    if (!state_exists && s->old.exists && s->current.exists && !Same(s->old.hash, s->current.hash)) return false;
    if (state_exists && old_changed && current_changed && s->old.exists && s->current.exists &&
        !Same(s->old.hash, s->current.hash)) return false;
    const bool baseline_diverged = state_exists &&
        (bool(s->state.flags & 1) != bool(s->state.flags & 2) ||
         ((s->state.flags & 3) == 3 && !Same(s->state.old_hash, s->state.current_hash)));
    // A legacy script may have edited a stale copy after the user separately
    // edited the canonical file. Replacing the canonical file here would lose
    // that edit; require manual reconciliation instead.
    if (baseline_diverged && old_changed && !current_changed && s->old.exists && s->current.exists &&
        !Same(s->old.hash, s->current.hash)) return false;
    if (s->old.exists && (!s->current.exists || (old_changed && !current_changed))) {
        // A deletion of an existing canonical file is not interpreted as an
        // instruction to resurrect old settings without confirmation.
        if (state_exists && !s->current.exists && (s->state.flags & 2)) return false;
        if (!InstallLegacy(*s) || !Read(Current, &s->current)) return false;
    } else if (!SaveState(s->old, s->current)) return false;
    if (s->current.exists) { std::memcpy(out, s->current.bytes, s->current.size); *actual = s->current.size; }
    return true;
}
}
