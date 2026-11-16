#include "storage_migration.hpp"
#include "../../../common/roomcircuit_paths.h"
#include "../../../common/game_identity_sha256.h"
#include <stratosphere.hpp>
#include <algorithm>
#include <cstring>

namespace ams::mitm::storage {
namespace {
constexpr const char *Stage = RC_CONFIG_DIR "/.data-v2-staging";
constexpr const char *Owner = RC_CONFIG_DIR "/.data-v2-staging/.roomcircuit-owned";
constexpr const char *StageMarker = RC_CONFIG_DIR "/.data-v2-staging/.storage-v2";
constexpr const char *Journal = RC_LEGACY_PATH(".update/journal");
constexpr const char *Bootstrap = RC_CONFIG_DIR "/config.ini";
constexpr const char *BootstrapV1 = RC_V1_CONFIG_DIR "/config.ini";
constexpr const char *BootstrapWork = RC_CONFIG_DIR "/.bootstrap-sync";
constexpr const char *BootstrapOwner = RC_CONFIG_DIR "/.bootstrap-sync/owner";
constexpr const char *BootstrapTemp = RC_CONFIG_DIR "/.bootstrap-sync/config.tmp";
constexpr const char *BootstrapJournal = RC_CONFIG_DIR "/.bootstrap-sync/journal";
constexpr const char *BootstrapJournalTemp = RC_CONFIG_DIR "/.bootstrap-sync/journal.tmp";
constexpr s64 MaxUserFile = 8 * 1024 * 1024;
Selection selection = Selection::Unavailable;
unsigned health = 0;
bool bootstrap_conflict = false, bootstrap_check_failed = false;
constexpr const char *BaselineStage = RC_CONFIG_DIR "/.data-v2-staging/.v1-baseline";
constexpr const char *BaselinePath = RC_CURRENT_DIR "/.v1-baseline";
constexpr const char *Critical[] = {"config.ini", "config.ini.bak", "selected-node.bin", "selected-node.bin.previous",
    "appointment-subscriptions.bin", "appointment-subscriptions.bin.previous", "language.ini", "language.ini.previous"};
struct Baseline { u8 magic[4]; u8 v1_present; u8 reserved[3]; u32 present; u8 hashes[8][32]; u8 checksum[32]; };
struct BootstrapRecord { u8 magic[4]; u8 hash[32]; u8 checksum[32]; };
void Hash(const void *data, size_t size, u8 out[32]) {
    GameIdentitySha256 sha; gi_sha256_init(&sha); gi_sha256_update(&sha, data, size); gi_sha256_final(&sha, out);
}

// Configuration and recovery sidecars must move together. Transaction files
// intentionally stay at the v1 updater's fixed location until acknowledged.
constexpr const char *UserFiles[] = {
    "config.ini", "config.ini.bak", "language.ini", "language.ini.previous",
    "appointment-subscriptions.bin", "appointment-subscriptions.bin.previous",
    "game-titles.bin", "game-titles.bin.previous", "lanplay-ip.bin", "lanplay-ip.bin.previous",
    "selected-node.bin", "selected-node.bin.previous",
    "debug.flag", "captura.flag", "icon-status.log", "node-selection-app.log", "node-selection.log",
    "relay.log", "bsd.log", "ldn_mitm.log",
};

bool Exists(const char *path, bool *exists) {
    fs::DirectoryEntryType type;
    const Result result = fs::GetEntryType(&type, path);
    if (fs::ResultPathNotFound::Includes(result)) { *exists = false; return true; }
    if (R_FAILED(result) || type != fs::DirectoryEntryType_File) return false;
    *exists = true;
    return true;
}

bool EmptyDirectory(const char *path) {
    fs::DirectoryHandle handle;
    if (R_FAILED(fs::OpenDirectory(&handle, path, fs::OpenDirectoryMode_All))) return false;
    s64 count = -1;
    const Result result = fs::GetDirectoryEntryCount(&count, handle);
    fs::CloseDirectory(handle);
    return R_SUCCEEDED(result) && count == 0;
}

bool RemoveOwnedFile(const char *path) {
    const Result result = fs::DeleteFile(path);
    return R_SUCCEEDED(result) || fs::ResultPathNotFound::Includes(result);
}

bool MakeMarker(const char *path) {
    if (R_FAILED(fs::CreateFile(path, 0))) return false;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, path, fs::OpenMode_Write))) return false;
    const Result result = fs::FlushFile(file);
    fs::CloseFile(file);
    return R_SUCCEEDED(result);
}

bool CopyAndVerify(const char *source, const char *destination) {
    fs::FileHandle input;
    if (R_FAILED(fs::OpenFile(&input, source, fs::OpenMode_Read))) return false;
    s64 size = 0;
    bool ok = R_SUCCEEDED(fs::GetFileSize(&size, input)) && size >= 0 && size <= MaxUserFile;
    if (ok) ok = R_SUCCEEDED(fs::CreateFile(destination, size));
    fs::FileHandle output{};
    bool opened = false;
    if (ok) { opened = R_SUCCEEDED(fs::OpenFile(&output, destination, fs::OpenMode_Write)); ok = opened; }
    u8 buffer[4096];
    for (s64 offset = 0; ok && offset < size;) {
        const size_t chunk = static_cast<size_t>(std::min<s64>(size - offset, sizeof(buffer)));
        size_t read = 0;
        ok = R_SUCCEEDED(fs::ReadFile(&read, input, offset, buffer, chunk)) && read == chunk &&
            R_SUCCEEDED(fs::WriteFile(output, offset, buffer, chunk, fs::WriteOption::None));
        offset += chunk;
    }
    if (opened) {
        if (ok) ok = R_SUCCEEDED(fs::FlushFile(output));
        fs::CloseFile(output);
    }
    if (ok) {
        fs::FileHandle check;
        if (R_FAILED(fs::OpenFile(&check, destination, fs::OpenMode_Read))) ok = false;
        else {
            s64 actual = -1;
            ok = R_SUCCEEDED(fs::GetFileSize(&actual, check)) && actual == size;
            u8 copy[4096];
            for (s64 offset = 0; ok && offset < size;) {
                const size_t chunk = static_cast<size_t>(std::min<s64>(size - offset, sizeof(buffer)));
                size_t a = 0, b = 0;
                ok = R_SUCCEEDED(fs::ReadFile(&a, input, offset, buffer, chunk)) && a == chunk &&
                    R_SUCCEEDED(fs::ReadFile(&b, check, offset, copy, chunk)) && b == chunk &&
                    std::memcmp(buffer, copy, chunk) == 0;
                offset += chunk;
            }
            fs::CloseFile(check);
        }
    }
    fs::CloseFile(input);
    return ok;
}

bool HashFile(const char *path, bool *exists, u8 digest[32]) {
    if (!Exists(path, exists)) return false;
    if (!*exists) return true;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, path, fs::OpenMode_Read))) return false;
    s64 size = -1;
    bool ok = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size >= 0 && size <= MaxUserFile;
    GameIdentitySha256 sha; gi_sha256_init(&sha); u8 buffer[1024];
    for (s64 offset = 0; ok && offset < size;) {
        const size_t chunk = static_cast<size_t>(std::min<s64>(size - offset, sizeof(buffer)));
        size_t actual = 0;
        ok = R_SUCCEEDED(fs::ReadFile(&actual, file, offset, buffer, chunk)) && actual == chunk;
        if (ok) gi_sha256_update(&sha, buffer, chunk);
        offset += chunk;
    }
    fs::CloseFile(file);
    if (ok) gi_sha256_final(&sha, digest);
    return ok;
}

bool EnsureBootstrapWork() {
    bool dir = false, owner = false;
    if (R_FAILED(fs::HasDirectory(&dir, BootstrapWork))) return false;
    if (!dir && R_FAILED(fs::CreateDirectory(BootstrapWork))) return false;
    if (!Exists(BootstrapOwner, &owner)) return false;
    if (owner) return true;
    return EmptyDirectory(BootstrapWork) && MakeMarker(BootstrapOwner);
}

bool BootstrapStampPath(char out[256], const u8 digest[32]) {
    int n = util::TSNPrintf(out, 256, "%s/used-", BootstrapWork);
    if (n <= 0 || n >= 256) return false;
    for (unsigned i = 0; i < 32; ++i) {
        const int written = util::TSNPrintf(out + n, 256 - n, "%02x", digest[i]);
        if (written != 2 || n + written >= 256) return false;
        n += written;
    }
    return true;
}

bool MarkBootstrapUsed(const u8 digest[32]) {
    char stamp[256]; bool exists = false;
    if (!BootstrapStampPath(stamp, digest) || !Exists(stamp, &exists)) return false;
    return exists || MakeMarker(stamp);
}

bool ReadBootstrapJournal(BootstrapRecord *record, bool *exists) {
    if (!Exists(BootstrapJournal, exists)) return false;
    if (!*exists) return true;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, BootstrapJournal, fs::OpenMode_Read))) return false;
    s64 size = -1; size_t actual = 0;
    const bool ok = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size == static_cast<s64>(sizeof(*record)) &&
        R_SUCCEEDED(fs::ReadFile(&actual, file, 0, record, sizeof(*record))) && actual == sizeof(*record);
    fs::CloseFile(file);
    u8 checksum[32]; Hash(record, offsetof(BootstrapRecord, checksum), checksum);
    return ok && !std::memcmp(record->magic, "RCB2", 4) && !std::memcmp(record->checksum, checksum, 32);
}

bool WriteBootstrapJournal(const u8 digest[32]) {
    BootstrapRecord record{}; std::memcpy(record.magic, "RCB2", 4); std::memcpy(record.hash, digest, 32);
    Hash(&record, offsetof(BootstrapRecord, checksum), record.checksum);
    if (!RemoveOwnedFile(BootstrapJournalTemp) || R_FAILED(fs::CreateFile(BootstrapJournalTemp, sizeof(record)))) return false;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, BootstrapJournalTemp, fs::OpenMode_Write))) return false;
    const bool ok = R_SUCCEEDED(fs::WriteFile(file, 0, &record, sizeof(record), fs::WriteOption::Flush));
    fs::CloseFile(file);
    if (!ok) return false;
    // A journal is installed before any destination is touched. A failed
    // rename leaves only our temporary file and is safe to retry.
    if (R_FAILED(fs::RenameFile(BootstrapJournalTemp, BootstrapJournal))) return false;
    BootstrapRecord verified{}; bool exists = false;
    return ReadBootstrapJournal(&verified, &exists) && exists && !std::memcmp(&verified, &record, sizeof(record));
}

bool FindBootstrap(const char **source, bool *exists, u8 digest[32]) {
    bool current = false, old = false; u8 first[32]{}, second[32]{};
    if (!HashFile(Bootstrap, &current, first) || !HashFile(BootstrapV1, &old, second)) {
        bootstrap_check_failed = true;
        return false;
    }
    if (current && old && std::memcmp(first, second, sizeof(first))) {
        bootstrap_conflict = true;
        return false;
    }
    *exists = current || old;
    if (*exists) {
        *source = current ? Bootstrap : BootstrapV1;
        std::memcpy(digest, current ? first : second, 32);
    }
    return true;
}

// When an authoritative config already exists, stale package bootstraps are
// not candidates for migration. Remember each separately so deleting the
// authoritative file later cannot accidentally resurrect either old value.
bool IgnoreExistingBootstraps() {
    bool current = false, old = false; u8 first[32]{}, second[32]{};
    if (!HashFile(Bootstrap, &current, first) || !HashFile(BootstrapV1, &old, second)) return false;
    if (!current && !old) return true;
    if (!EnsureBootstrapWork()) return false;
    if ((current && !MarkBootstrapUsed(first)) || (old && !MarkBootstrapUsed(second))) return false;
    // An interrupted prior import is superseded by the existing user config.
    // Clean only a temporary file that matches the verified owned journal.
    BootstrapRecord journal{}; bool has_journal = false;
    if (!ReadBootstrapJournal(&journal, &has_journal)) return false;
    if (has_journal) {
        bool temp = false; u8 hash[32]{};
        if (!HashFile(BootstrapTemp, &temp, hash) || (temp && std::memcmp(hash, journal.hash, 32))) return false;
        if (!MarkBootstrapUsed(journal.hash) || (temp && !RemoveOwnedFile(BootstrapTemp)) ||
            !RemoveOwnedFile(BootstrapJournal)) return false;
    }
    return true;
}

bool ReconcileBootstrapImpl() {
    bool pending = false;
    if (!Exists(Journal, &pending)) return false;
    if (pending) return true;
    bool existing = false, backup = false;
    if (!Exists(RC_CURRENT_PATH("config.ini"), &existing) ||
        !Exists(RC_CURRENT_PATH("config.ini.bak"), &backup)) return false;
    const char *source = nullptr; bool have_bootstrap = false; u8 desired[32]{};
    const bool known = FindBootstrap(&source, &have_bootstrap, desired);
    // Never replace an existing primary or recovery configuration. If the
    // old and new bootstrap disagree, leave both untouched.
    if (!known && (existing || backup)) {
        if (bootstrap_conflict) {
            bootstrap_conflict = false; // Existing user data is authoritative.
            if (!IgnoreExistingBootstraps()) { bootstrap_check_failed = true; return false; }
        }
        return true;
    }
    if (!known) return false;
    bool journal_present = false;
    if (!Exists(BootstrapJournal, &journal_present)) return false;
    if (!have_bootstrap && !journal_present) return true;
    if (!EnsureBootstrapWork()) return false;
    BootstrapRecord journal{}; bool has_journal = false;
    if (!ReadBootstrapJournal(&journal, &has_journal)) return false;
    if (has_journal) {
        bool temp = false; u8 temp_hash[32]{};
        if (!HashFile(BootstrapTemp, &temp, temp_hash)) return false;
        if (temp && std::memcmp(temp_hash, journal.hash, 32)) {
            // An interrupted copy can leave a partial owned temporary file.
            // Discard it only while an intact source still matches the journal.
            if (!have_bootstrap || std::memcmp(desired, journal.hash, 32) || !RemoveOwnedFile(BootstrapTemp)) return false;
            temp = false;
        }
        if (!existing && !backup) {
            // If a user removed or replaced the package bootstrap while the
            // transaction was interrupted, retain our temp and do not
            // resurrect a configuration they may have intentionally removed.
            if (!have_bootstrap || std::memcmp(desired, journal.hash, 32)) return false;
            if (!temp) {
                if (!CopyAndVerify(source, BootstrapTemp)) return false;
                bool source_exists = false; u8 source_hash[32]{};
                if (!HashFile(source, &source_exists, source_hash) || !source_exists ||
                    std::memcmp(source_hash, journal.hash, 32)) return false;
            }
            // RenameFile refuses an existing destination; concurrent config
            // creation is never overwritten.
            if (R_FAILED(fs::RenameFile(BootstrapTemp, RC_CURRENT_PATH("config.ini")))) return false;
            temp = false;
        }
        if (!MarkBootstrapUsed(journal.hash) || (temp && !RemoveOwnedFile(BootstrapTemp)) ||
            !RemoveOwnedFile(BootstrapJournal)) return false;
        existing = true;
    }
    if (!have_bootstrap) return true;
    char stamp[256]; bool used = false;
    if (!BootstrapStampPath(stamp, desired) || !Exists(stamp, &used)) return false;
    if (existing || backup) return used || MarkBootstrapUsed(desired);
    if (used) return true; // User removed an already-consumed configuration.
    if (!RemoveOwnedFile(BootstrapJournalTemp) || !RemoveOwnedFile(BootstrapTemp) ||
        !WriteBootstrapJournal(desired) || !CopyAndVerify(source, BootstrapTemp)) return false;
    bool current = false, saved = false, source_exists = false; u8 source_hash[32]{}, temp_hash[32]{};
    if (!Exists(RC_CURRENT_PATH("config.ini"), &current) || !Exists(RC_CURRENT_PATH("config.ini.bak"), &saved) ||
        !HashFile(source, &source_exists, source_hash) || !source_exists || std::memcmp(source_hash, desired, 32) ||
        !HashFile(BootstrapTemp, &source_exists, temp_hash) || !source_exists || std::memcmp(temp_hash, desired, 32)) return false;
    if (current || saved) return false; // Preserve a concurrent user write.
    if (R_FAILED(fs::RenameFile(BootstrapTemp, RC_CURRENT_PATH("config.ini")))) return false;
    return MarkBootstrapUsed(desired) && RemoveOwnedFile(BootstrapJournal);
}

bool ReconcileBootstrap() {
    const bool ok = ReconcileBootstrapImpl();
    if (!ok && !bootstrap_conflict) bootstrap_check_failed = true;
    return ok;
}

bool ReadBaseline(const char *path, Baseline *out) {
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, path, fs::OpenMode_Read))) return false;
    s64 size = -1; size_t actual = 0;
    const bool read = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size == static_cast<s64>(sizeof(*out)) &&
        R_SUCCEEDED(fs::ReadFile(&actual, file, 0, out, sizeof(*out))) && actual == sizeof(*out);
    fs::CloseFile(file);
    if (!read) return false;
    u8 digest[32]; Hash(out, offsetof(Baseline, checksum), digest);
    return !std::memcmp(out->magic, "RCV1", 4) && out->v1_present <= 1 &&
        !out->reserved[0] && !out->reserved[1] && !out->reserved[2] &&
        !(out->present & ~((1u << 8) - 1)) && !std::memcmp(digest, out->checksum, 32);
}

bool StoreBaseline(bool v1) {
    Baseline base{}; std::memcpy(base.magic, "RCV1", 4); base.v1_present = v1;
    for (size_t i = 0; v1 && i < sizeof(Critical) / sizeof(Critical[0]); ++i) {
        char path[256]; const int n = util::TSNPrintf(path, sizeof(path), "%s/%s", Stage, Critical[i]);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return false;
        bool exists = false;
        if (!HashFile(path, &exists, base.hashes[i])) return false;
        if (exists) base.present |= 1u << i;
    }
    Hash(&base, offsetof(Baseline, checksum), base.checksum);
    if (!RemoveOwnedFile(BaselineStage) || R_FAILED(fs::CreateFile(BaselineStage, sizeof(base)))) return false;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, BaselineStage, fs::OpenMode_Write))) return false;
    const bool written = R_SUCCEEDED(fs::WriteFile(file, 0, &base, sizeof(base), fs::WriteOption::Flush));
    fs::CloseFile(file);
    Baseline verified{};
    return written && ReadBaseline(BaselineStage, &verified) && !std::memcmp(&base, &verified, sizeof(base));
}

unsigned CheckLegacyChanges() {
    bool v1_dir = false, marker = false;
    if (R_FAILED(fs::HasDirectory(&v1_dir, RC_V1_DIR))) return RC_STORAGE_HEALTH_LEGACY_UNCHECKED;
    if (!v1_dir) return 0; // Removing the old homebrew folder is normal.
    if (R_FAILED(fs::HasFile(&marker, RC_V1_MARKER))) return RC_STORAGE_HEALTH_LEGACY_UNCHECKED;
    if (!marker) return RC_STORAGE_HEALTH_LEGACY_CHANGED;
    Baseline base{};
    if (!ReadBaseline(BaselinePath, &base)) return RC_STORAGE_HEALTH_LEGACY_UNCHECKED;
    if (!base.v1_present) return RC_STORAGE_HEALTH_LEGACY_CHANGED;
    for (size_t i = 0; i < sizeof(Critical) / sizeof(Critical[0]); ++i) {
        char path[256]; const int n = util::TSNPrintf(path, sizeof(path), "%s/%s", RC_V1_DIR, Critical[i]);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return RC_STORAGE_HEALTH_LEGACY_UNCHECKED;
        bool exists = false; u8 digest[32]{};
        if (!HashFile(path, &exists, digest)) return RC_STORAGE_HEALTH_LEGACY_UNCHECKED;
        if (exists != bool(base.present & (1u << i)) || (exists && std::memcmp(digest, base.hashes[i], 32)))
            return RC_STORAGE_HEALTH_LEGACY_CHANGED;
    }
    return 0;
}
}

static bool Pair(const char *marker_path, const char *directory_path, bool *present) {
    bool marker = false, directory = false;
    if (R_FAILED(fs::HasFile(&marker, marker_path)) ||
        R_FAILED(fs::HasDirectory(&directory, directory_path)) || marker != directory) return false;
    *present = marker;
    return true;
}

static bool AttemptMigration() {
    bool marker = false, directory = false;
    if (R_FAILED(fs::HasFile(&marker, RC_STORAGE_MARKER)) ||
        R_FAILED(fs::HasDirectory(&directory, RC_CURRENT_DIR))) return false;
    if (marker && directory) return ReconcileBootstrap();
    if (directory || marker) return false; // Preserve existing user data on conflict.

    bool pending = false, v1 = false;
    if (!Exists(Journal, &pending) || pending) return false;
    if (!Pair(RC_V1_MARKER, RC_V1_DIR, &v1)) return false;
    if (R_FAILED(fs::EnsureDirectory(RC_CONFIG_DIR))) return false;
    bool stage_exists = false;
    if (R_FAILED(fs::HasDirectory(&stage_exists, Stage))) return false;
    if (!stage_exists && R_FAILED(fs::CreateDirectory(Stage))) return false;
    bool owned = false;
    if (!Exists(Owner, &owned)) return false;
    if (!owned) {
        if (!EmptyDirectory(Stage) || !MakeMarker(Owner)) return false;
    }
    if (!RemoveOwnedFile(StageMarker)) return false;
    char primary_path[256], backup_path[256];
    const int primary_length = util::TSNPrintf(primary_path, sizeof(primary_path), "%s/config.ini", v1 ? RC_V1_DIR : RC_LEGACY_DIR);
    const int backup_length = util::TSNPrintf(backup_path, sizeof(backup_path), "%s/config.ini.bak", v1 ? RC_V1_DIR : RC_LEGACY_DIR);
    bool source_primary = false, source_backup = false;
    if (primary_length <= 0 || static_cast<size_t>(primary_length) >= sizeof(primary_path) ||
        backup_length <= 0 || static_cast<size_t>(backup_length) >= sizeof(backup_path) ||
        !Exists(primary_path, &source_primary) || !Exists(backup_path, &source_backup)) return false;
    const char *bootstrap_source = nullptr; bool bootstrap_exists = false; u8 bootstrap_hash[32]{};
    if (!FindBootstrap(&bootstrap_source, &bootstrap_exists, bootstrap_hash)) {
        if ((!source_primary && !source_backup) || !bootstrap_conflict) return false;
        bootstrap_conflict = false; // Existing user data is authoritative.
        if (!IgnoreExistingBootstraps()) { bootstrap_check_failed = true; return false; }
    }
    for (const char *name : UserFiles) {
        char old_path[256], stage_path[256];
        const int old_length = util::TSNPrintf(old_path, sizeof(old_path), "%s/%s", v1 ? RC_V1_DIR : RC_LEGACY_DIR, name);
        const int stage_length = util::TSNPrintf(stage_path, sizeof(stage_path), "%s/%s", Stage, name);
        if (old_length <= 0 || static_cast<size_t>(old_length) >= sizeof(old_path) ||
            stage_length <= 0 || static_cast<size_t>(stage_length) >= sizeof(stage_path)) return false;
        bool exists = false;
        if (!Exists(old_path, &exists) || !RemoveOwnedFile(stage_path)) return false;
        const char *source = old_path;
        if (std::strcmp(name, "config.ini") == 0) {
            // Explicit server ZIPs can provide a first-install bootstrap.
            // A source primary or recovery config always remains authoritative.
            if (bootstrap_exists) {
                if (!exists && !source_backup) { source = bootstrap_source; exists = true; }
            }
        }
        if (exists && !CopyAndVerify(source, stage_path)) return false;
    }
    // Record bootstrap consumption before committing the data directory, so
    // a later user deletion cannot resurrect an old package configuration.
    if (bootstrap_exists && (!EnsureBootstrapWork() || !MarkBootstrapUsed(bootstrap_hash))) return false;
    if (!StoreBaseline(v1) || !MakeMarker(StageMarker) || R_FAILED(fs::RenameDirectory(Stage, RC_CURRENT_DIR))) return false;
    return ReconcileBootstrap();
}

bool Initialize() {
    selection = Selection::Unavailable; health = 0;
    bootstrap_conflict = false; bootstrap_check_failed = false;
    (void)AttemptMigration();
    // A failed operation can leave the rename's result ambiguous. Re-read the
    // shared commit marker before choosing the runtime root. Never direct writes
    // to the old data if the already-migrated marker cannot be checked.
    bool current = false, v1 = false;
    if (!Pair(RC_STORAGE_MARKER, RC_CURRENT_DIR, &current)) return false;
    if (current) { selection = Selection::Current; health = CheckLegacyChanges(); }
    else {
        if (!Pair(RC_V1_MARKER, RC_V1_DIR, &v1)) return false;
        selection = v1 ? Selection::V1 : Selection::Legacy;
    }
    if (bootstrap_check_failed) health |= RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED;
    if (bootstrap_conflict) {
        const char *primary = selection == Selection::Current ? RC_CURRENT_PATH("config.ini") :
            selection == Selection::V1 ? RC_V1_PATH("config.ini") : RC_LEGACY_PATH("config.ini");
        const char *backup = selection == Selection::Current ? RC_CURRENT_PATH("config.ini.bak") :
            selection == Selection::V1 ? RC_V1_PATH("config.ini.bak") : RC_LEGACY_PATH("config.ini.bak");
        bool has_primary = false, has_backup = false;
        if (!Exists(primary, &has_primary) || !Exists(backup, &has_backup))
            health |= RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED;
        else if (selection != Selection::Current || (!has_primary && !has_backup))
            health |= RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT;
    }
    return selection == Selection::Current;
}
Selection StorageSelection() { return selection; }
bool StorageUseCurrent() { return selection == Selection::Current; }
unsigned StorageGeneration() { return static_cast<unsigned>(selection); }
unsigned StorageHealth() { return health; }
bool StorageAvailable() { return selection != Selection::Unavailable; }
}
