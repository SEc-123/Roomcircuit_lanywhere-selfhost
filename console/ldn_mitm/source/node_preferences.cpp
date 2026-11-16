// SPDX-License-Identifier: GPL-2.0-only
#include "node_preferences.hpp"
#include "storage_migration.hpp"
#include "../../../common/roomcircuit_paths.h"
#include <stratosphere.hpp>
#include <cstring>

namespace ams::mitm::lanywhere {
namespace {
struct Record {
    u32 magic, version;
    NodeDynamicEntry node;
    u8 hash[32];
};
constexpr u32 Magic = 0x524e4f44;
const char *Path() { return rc_storage_path(storage::StorageUseCurrent(), RC_LEGACY_PATH("selected-node.bin"), RC_CURRENT_PATH("selected-node.bin")); }
const char *Previous() { return rc_storage_path(storage::StorageUseCurrent(), RC_LEGACY_PATH("selected-node.bin.previous"), RC_CURRENT_PATH("selected-node.bin.previous")); }
const char *Invalid() { return rc_storage_path(storage::StorageUseCurrent(), RC_LEGACY_PATH("selected-node.bin.invalid"), RC_CURRENT_PATH("selected-node.bin.invalid")); }
const char *Temporary() { return rc_storage_path(storage::StorageUseCurrent(), RC_LEGACY_PATH("selected-node.bin.tmp"), RC_CURRENT_PATH("selected-node.bin.tmp")); }
bool RemoveIfPresent(const char *path) {
    const Result r = fs::DeleteFile(path);
    return R_SUCCEEDED(r) || fs::ResultPathNotFound::Includes(r);
}
NodePreferenceRead Read(const char *path, Record *record) {
    fs::FileHandle file;
    const Result opened = fs::OpenFile(&file, path, fs::OpenMode_Read);
    if (fs::ResultPathNotFound::Includes(opened)) return NodePreferenceRead::Missing;
    if (R_FAILED(opened)) return NodePreferenceRead::Invalid;
    s64 size = 0; size_t actual = 0;
    bool ok = R_SUCCEEDED(fs::GetFileSize(&size, file)) && size == sizeof(*record) &&
        R_SUCCEEDED(fs::ReadFile(&actual, file, 0, record, sizeof(*record))) && actual == sizeof(*record);
    fs::CloseFile(file);
    if (!ok || record->magic != Magic || record->version != 1) return NodePreferenceRead::Invalid;
    u8 hash[32]; crypto::GenerateSha256(hash, sizeof(hash), record, offsetof(Record, hash));
    return std::memcmp(hash, record->hash, sizeof(hash)) == 0 ? NodePreferenceRead::Valid : NodePreferenceRead::Invalid;
}
}
NodePreferenceRead ReadNodePreference(NodeDynamicEntry *entry) {
    if (!entry) return NodePreferenceRead::Invalid;
    Record record{};
    const auto primary = Read(Path(), &record);
    if (primary == NodePreferenceRead::Valid) { *entry = record.node; return primary; }
    const auto previous = Read(Previous(), &record);
    if (previous == NodePreferenceRead::Valid) { *entry = record.node; return previous; }
    return primary == NodePreferenceRead::Missing && previous == NodePreferenceRead::Missing ?
        NodePreferenceRead::Missing : NodePreferenceRead::Invalid;
}
bool SaveNodePreference(const NodeDynamicEntry &entry) {
    Record saved{Magic, 1, entry, {}};
    crypto::GenerateSha256(saved.hash, sizeof(saved.hash), &saved, offsetof(Record, hash));
    const char *directory = storage::StorageUseCurrent() ? RC_CURRENT_DIR : RC_LEGACY_DIR;
    static_cast<void>(fs::CreateDirectory(directory));
    if (!RemoveIfPresent(Temporary()) || R_FAILED(fs::CreateFile(Temporary(), sizeof(saved)))) return false;
    fs::FileHandle file;
    bool ok = false;
    if (R_SUCCEEDED(fs::OpenFile(&file, Temporary(), fs::OpenMode_Write))) {
        ok = R_SUCCEEDED(fs::WriteFile(file, 0, &saved, sizeof(saved), fs::WriteOption::Flush));
        fs::CloseFile(file);
    }
    Record verify{};
    ok = ok && Read(Temporary(), &verify) == NodePreferenceRead::Valid && std::memcmp(&saved, &verify, sizeof(saved)) == 0;
    Record old{};
    bool moved = false, moved_invalid = false;
    const auto original = Read(Path(), &old);
    if (ok && original == NodePreferenceRead::Valid) {
        ok = RemoveIfPresent(Previous()) && R_SUCCEEDED(fs::RenameFile(Path(), Previous()));
        moved = ok;
    } else if (ok && original == NodePreferenceRead::Invalid) {
        // Retain damaged preferences for recovery rather than deleting them.
        bool exists = false;
        ok = R_SUCCEEDED(fs::HasFile(&exists, Invalid())) && !exists && R_SUCCEEDED(fs::RenameFile(Path(), Invalid()));
        moved_invalid = ok;
    }
    if (ok) ok = R_SUCCEEDED(fs::RenameFile(Temporary(), Path()));
    if (!ok) {
        if (moved) static_cast<void>(fs::RenameFile(Previous(), Path()));
        if (moved_invalid) static_cast<void>(fs::RenameFile(Invalid(), Path()));
        static_cast<void>(fs::DeleteFile(Temporary()));
    }
    return ok;
}
}
