#include "storage_migration.hpp"
#include "../../../common/roomcircuit_paths.h"
#include <stratosphere.hpp>
#include <algorithm>
#include <cstring>

namespace ams::mitm::storage {
namespace {
constexpr const char *Stage = RC_CONFIG_DIR "/.data-v1-staging";
constexpr const char *Owner = RC_CONFIG_DIR "/.data-v1-staging/.roomcircuit-owned";
constexpr const char *StageMarker = RC_CONFIG_DIR "/.data-v1-staging/.storage-v1";
constexpr const char *Journal = RC_LEGACY_PATH(".update/journal");
constexpr s64 MaxUserFile = 8 * 1024 * 1024;
Selection selection = Selection::Unavailable;

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

bool FilesEqual(const char *left, const char *right) {
    fs::FileHandle a;
    if (R_FAILED(fs::OpenFile(&a, left, fs::OpenMode_Read))) return false;
    fs::FileHandle b;
    if (R_FAILED(fs::OpenFile(&b, right, fs::OpenMode_Read))) { fs::CloseFile(a); return false; }
    s64 a_size = -1, b_size = -1;
    bool equal = R_SUCCEEDED(fs::GetFileSize(&a_size, a)) && R_SUCCEEDED(fs::GetFileSize(&b_size, b)) &&
        a_size >= 0 && a_size <= MaxUserFile && a_size == b_size;
    u8 a_bytes[1024], b_bytes[1024];
    for (s64 offset = 0; equal && offset < a_size;) {
        const size_t chunk = static_cast<size_t>(std::min<s64>(a_size - offset, sizeof(a_bytes)));
        size_t a_read = 0, b_read = 0;
        equal = R_SUCCEEDED(fs::ReadFile(&a_read, a, offset, a_bytes, chunk)) && a_read == chunk &&
            R_SUCCEEDED(fs::ReadFile(&b_read, b, offset, b_bytes, chunk)) && b_read == chunk &&
            std::memcmp(a_bytes, b_bytes, chunk) == 0;
        offset += chunk;
    }
    fs::CloseFile(b); fs::CloseFile(a);
    return equal;
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
}

static bool AttemptMigration() {
    bool marker = false, directory = false;
    if (R_FAILED(fs::HasFile(&marker, RC_STORAGE_MARKER)) ||
        R_FAILED(fs::HasDirectory(&directory, RC_CURRENT_DIR))) return false;
    if (marker && directory) return true;
    if (directory || marker) return false; // Preserve existing user data on conflict.

    bool pending = false;
    if (!Exists(Journal, &pending) || pending) return false;
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
    for (const char *name : UserFiles) {
        char old_path[256], stage_path[256];
        const int old_length = util::TSNPrintf(old_path, sizeof(old_path), "%s/%s", RC_LEGACY_DIR, name);
        const int stage_length = util::TSNPrintf(stage_path, sizeof(stage_path), "%s/%s", Stage, name);
        if (old_length <= 0 || static_cast<size_t>(old_length) >= sizeof(old_path) ||
            stage_length <= 0 || static_cast<size_t>(stage_length) >= sizeof(stage_path)) return false;
        bool exists = false;
        if (!Exists(old_path, &exists) || !RemoveOwnedFile(stage_path)) return false;
        const char *source = old_path;
        if (std::strcmp(name, "config.ini") == 0) {
            // Explicit server ZIPs can provide a first-install bootstrap
            // config. Never overwrite either side if an older config differs.
            constexpr const char *Bootstrap = RC_CONFIG_DIR "/config.ini";
            bool bootstrap_exists = false;
            if (!Exists(Bootstrap, &bootstrap_exists)) return false;
            if (bootstrap_exists) {
                if (exists && !FilesEqual(old_path, Bootstrap)) return false;
                if (!exists) { source = Bootstrap; exists = true; }
            }
        }
        if (exists && !CopyAndVerify(source, stage_path)) return false;
    }
    if (!MakeMarker(StageMarker) || R_FAILED(fs::RenameDirectory(Stage, RC_CURRENT_DIR))) return false;
    return true;
}

bool Initialize() {
    selection = Selection::Unavailable;
    (void)AttemptMigration();
    // A failed operation can leave the rename's result ambiguous. Re-read the
    // shared commit marker before choosing the runtime root. Never direct writes
    // to the old data if the already-migrated marker cannot be checked.
    bool marker = false, directory = false;
    if (R_FAILED(fs::HasFile(&marker, RC_STORAGE_MARKER)) ||
        R_FAILED(fs::HasDirectory(&directory, RC_CURRENT_DIR)) || (marker != directory)) return false;
    selection = marker ? Selection::Current : Selection::Legacy;
    return selection == Selection::Current;
}
Selection StorageSelection() { return selection; }
bool StorageUseCurrent() { return selection == Selection::Current; }
bool StorageAvailable() { return selection != Selection::Unavailable; }
}
