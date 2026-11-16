#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <sodium.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#include "../../common/update_format.h"

static uint8_t test_key[32], secret_key[64];
static char test_root[4096];
static unsigned fault_at, mutation_count;
static bool inject_fail, cleanup_fault, owner_fault, directory_fault;
static uint64_t fake_available;
static unsigned last_progress;
static int injected_statvfs(const char *path, struct statvfs *info) {
    int result = statvfs(path, info);
    if (result == 0 && fake_available) { info->f_frsize = 1; info->f_bavail = fake_available; }
    return result;
}
static int injected_fsync(int fd) {
    struct stat info;
    if (directory_fault && fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) && ++mutation_count == fault_at) {
        if (inject_fail) { errno = EIO; return -1; }
        _exit(99);
    }
    return fsync(fd);
}
static bool managed(const char *path) {
    size_t length = strlen(test_root);
    if (strncmp(path, test_root, length) || path[length] != '/') return false;
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++)
        if (!strcmp(path + length + 1, LanywhereUpdateFiles[i])) return true;
    return !strcmp(path + length + 1, "switch/RoomCircuit/RoomCircuit.nro") ||
           !strcmp(path + length + 1, "switch/.overlays/RoomCircuit.ovl");
}
static bool mutation(const char *path) {
    if (owner_fault && !strcmp(path + strlen(test_root), "/switch/RoomCircuit/.entrypoints-owned-v1") && ++mutation_count == fault_at) {
        if (inject_fail) return true;
        _exit(99);
    }
    if (cleanup_fault && (!strcmp(path + strlen(test_root), "/lanywhere/.update/journal") ||
        !strcmp(path + strlen(test_root), "/switch/lanywhere.nro") ||
        !strcmp(path + strlen(test_root), "/switch/.overlays/lanywhere.ovl") ||
        !strcmp(path + strlen(test_root), "/switch/RoomCircuit/.legacy-cleanup-v1") ||
        !strcmp(path + strlen(test_root), "/switch/RoomCircuit/.entrypoints-owned-v1")) && ++mutation_count == fault_at)
        _exit(99);
    if (managed(path) && ++mutation_count == fault_at) {
        if (inject_fail) return true;
        _exit(99);
    }
    return false;
}
static int switch_rename(const char *from, const char *to) {
    struct stat info;
    if (lstat(to, &info) == 0) { errno = EEXIST; return -1; }
    if (mutation(to)) { errno = EIO; return -1; }
    return rename(from, to);
}
static int injected_unlink(const char *path) {
    if (mutation(path)) { errno = EIO; return -1; }
    return unlink(path);
}
#define UPDATE_VERIFY_PUBLIC_KEY test_key
#define rename switch_rename
#define unlink injected_unlink
#define statvfs(...) injected_statvfs(__VA_ARGS__)
#define fsync(...) injected_fsync(__VA_ARGS__)
#include "../source/update_core.c"
#undef rename
#undef unlink
#undef statvfs
#undef fsync

static uint8_t manifest_bytes[UPDATE_MANIFEST_SIZE];
static uint8_t *bundle;
static size_t bundle_size;
static const char OLD_DATA[] = "old file data";
static char download_path[4096];

static void make_bundle(void) {
    size_t raw_size = 12 + UPDATE_FILE_COUNT * 40 + 2 * (128 + 64 + 0x4000) + UPDATE_FILE_COUNT * 32;
    uint8_t *raw = calloc(1, raw_size); assert(raw);
    memcpy(raw, "LNUB", 4); write32(raw + 4, 1); write32(raw + 8, UPDATE_FILE_COUNT);
    size_t at = 12;
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        uint32_t length = i == 20 ? 0 : i == 18 || i == 19 ? 128 + 64 + 0x4000 : 32;
        write32(raw + at, i); write32(raw + at + 4, length);
        uint8_t *data = raw + at + 40;
        if (i == 18 || i == 19) {
            memcpy(data + 16, "NRO0", 4); write32(data + 24, 128);
            memcpy(data + 128, "ASET", 4); write32(data + 128 + 24, 64); write32(data + 128 + 32, 0x4000);
            memcpy(data + 192, "LANywhere", 9); memcpy(data + 192 + 0x3060, "0.7.1", 6);
        } else if (i == 21) memcpy(data, "PFS0", 4);
        else if (length) snprintf((char *)data, length, "new managed file %u", i);
        digest(data, length, raw + at + 8); at += 40 + length;
    }
    uLongf compressed_size = compressBound(at); bundle = malloc(compressed_size); assert(bundle);
    assert(compress2(bundle, &compressed_size, raw, at, Z_BEST_COMPRESSION) == Z_OK);
    bundle_size = compressed_size;
    memset(manifest_bytes, 0, sizeof(manifest_bytes)); memcpy(manifest_bytes, "LNU1", 4);
    write32(manifest_bytes + 4, 1); write32(manifest_bytes + 8, 0); write32(manifest_bytes + 12, 7);
    write32(manifest_bytes + 16, 1); write32(manifest_bytes + 20, 1);
    write32(manifest_bytes + 24, compressed_size); write32(manifest_bytes + 28, at);
    write32(manifest_bytes + 32, UPDATE_FILE_COUNT); digest(bundle, bundle_size, manifest_bytes + 36);
    memcpy(manifest_bytes + 68, "New update", 10); memcpy(manifest_bytes + 228, "Chinese update", 14);
    assert(crypto_sign_detached(manifest_bytes + 448, NULL, manifest_bytes, 448, secret_key) == 0);
    free(raw);
}
static void prepare_root(const char *base, unsigned number, bool missing) {
    snprintf(test_root, sizeof(test_root), "%s/case-%u", base, number); assert(mkdir(test_root, 0700) == 0);
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        char path[PATH_MAX]; assert(safe_parent(test_root, LanywhereUpdateFiles[i], true));
        assert(path_join(path, test_root, LanywhereUpdateFiles[i]));
        if (!missing || i % 3) assert(write_file(path, OLD_DATA, sizeof(OLD_DATA)));
    }
    char settings[PATH_MAX]; assert(path_join(settings, test_root, "lanywhere/config.ini"));
    assert(write_file(settings, "user-settings", 13));
    assert(path_join(settings, test_root, "lanywhere/appointments.bin")); assert(write_file(settings, "saved-appointments", 18));
    assert(path_join(download_path, test_root, "download.bin")); assert(write_file(download_path, bundle, bundle_size));
}
static void create_known_renamed(const char *root, const char *relative, const char *version) {
    char path[PATH_MAX];
    assert(safe_parent(root, relative, true) && path_join(path, root, relative));
    const size_t size = 128 + 64 + 0x4000;
    uint8_t *data = calloc(1, size); assert(data);
    memcpy(data + 16, "NRO0", 4); write32(data + 24, 128);
    memcpy(data + 128, "ASET", 4); write32(data + 128 + 24, 64); write32(data + 128 + 32, 0x4000);
    memcpy(data + 192, "RoomCircuit", sizeof("RoomCircuit"));
    memcpy(data + 192 + 0x3060, version, strlen(version) + 1);
    assert(write_file(path, data, size)); free(data);
}
static void sign_known_entrypoints(const char *root, const char *version) {
    uint8_t record[156] = {0}; memcpy(record, "RCI1", 4);
    assert(strlen(version) < 16); memcpy(record + 4, version, strlen(version));
    for (unsigned i = 0; i < 2; i++) {
        char path[PATH_MAX]; uint32_t size; uint8_t hash[32];
        const unsigned source = 1 - i;
        assert(path_join(path, root, BridgePaths[i]) && file_digest(path, &size, hash));
        write32(record + 20 + source * 36, size); memcpy(record + 24 + source * 36, hash, 32);
    }
    assert(crypto_sign_detached(record + 92, NULL, record, 92, secret_key) == 0);
    char path[PATH_MAX]; assert(safe_parent(root, "switch/RoomCircuit/.entrypoints-signed", true));
    assert(path_join(path, root, "switch/RoomCircuit/.entrypoints-signed"));
    assert(write_file(path, record, sizeof(record)));
}
static void establish_signed_ownership(const char *root, const char *version) {
    sign_known_entrypoints(root, version); update_record_renamed_ownership(root, version);
    for (unsigned i = 0; i < 2; i++) {
        char path[PATH_MAX]; assert(path_join(path, root, BridgePaths[i]));
        assert(bridge_owned_matches(root, i, path));
    }
}
static void assert_old(bool missing) {
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        char path[PATH_MAX]; bool exists; assert(path_join(path, test_root, LanywhereUpdateFiles[i]));
        assert(regular_or_missing(path, &exists));
        if (missing && i % 3 == 0) { assert(!exists); continue; }
        FILE *f = fopen(path, "rb"); assert(f); char data[32] = {0};
        assert(fread(data, 1, sizeof(OLD_DATA), f) == sizeof(OLD_DATA)); assert(!memcmp(data, OLD_DATA, sizeof(OLD_DATA))); fclose(f);
    }
}
static void assert_settings(void) {
    char path[PATH_MAX], data[32] = {0}; assert(path_join(path, test_root, "lanywhere/config.ini"));
    FILE *f = fopen(path, "rb"); assert(f && fread(data, 1, 13, f) == 13 && !memcmp(data, "user-settings", 13)); fclose(f);
    assert(path_join(path, test_root, "lanywhere/appointments.bin")); f = fopen(path, "rb");
    assert(f && fread(data, 1, 18, f) == 18 && !memcmp(data, "saved-appointments", 18)); fclose(f);
}
static void assert_commit_before_complete(unsigned completed, unsigned total, void *context) {
    assert(total == UPDATE_FILE_COUNT && completed == last_progress + 1);
    last_progress = completed;
    if (completed == total) {
        Journal journal; bool found;
        assert(journal_read(context, &journal, &found) && found && journal.phase == COMMITTED);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2 && sodium_init() >= 0);
    uint8_t seed[32] = {7}; assert(crypto_sign_seed_keypair(test_key, secret_key, seed) == 0);
    make_bundle(); UpdateManifest manifest;
    assert(update_manifest_parse(manifest_bytes, "0.7.0", &manifest) == UPDATE_OK);
    assert(update_manifest_parse(manifest_bytes, "0.7.1", &manifest) == UPDATE_NO_UPDATE);
    assert(!strcmp(manifest.version, "0.7.1"));
    assert(update_manifest_parse(manifest_bytes, "0.7.2", &manifest) == UPDATE_OLDER);
    manifest_bytes[100] ^= 1; assert(update_manifest_parse(manifest_bytes, "0.7.0", &manifest) == UPDATE_SIGNATURE); manifest_bytes[100] ^= 1;
    assert(update_manifest_parse(manifest_bytes, "0.7.0", &manifest) == UPDATE_OK);

    prepare_root(argv[1], 0, false);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_transaction_pending(test_root) == UPDATE_RECOVERY_REQUIRED);
    assert(update_recover(test_root) == UPDATE_INSTALLED);
    char version[32]; assert(update_pending_version(test_root, version) == UPDATE_INSTALLED && !strcmp(version, "0.7.1"));
    assert(update_acknowledge(test_root) == UPDATE_OK && update_recover(test_root) == UPDATE_OK);
    assert(update_transaction_pending(test_root) == UPDATE_OK);
    assert_settings();


    // A v0.8.9 updater installs the new binaries at the original 22 v1 IDs.
    // A new process promotes the two entrypoints only after the signed legacy
    // transaction was committed, and removes only its matching legacy files.
    prepare_root(argv[1], 700, false);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_promote_renamed(test_root, false) == UPDATE_OK);
    for (unsigned i = 0; i < 2; i++) {
        char target[PATH_MAX], original[PATH_MAX]; uint32_t size; uint8_t hash[32];
        assert(path_join(target, test_root, BridgePaths[i]));
        assert(path_join(original, test_root, LanywhereUpdateFiles[BridgeIds[i]]));
        assert(file_digest(original, &size, hash) && matches_file(target, size, hash));
    }
    assert(update_acknowledge(test_root) == UPDATE_OK);
    for (unsigned i = 0; i < 2; i++) {
        char original[PATH_MAX]; bool exists;
        assert(path_join(original, test_root, LanywhereUpdateFiles[BridgeIds[i]]));
        assert(regular_or_missing(original, &exists) && !exists);
    }
    assert_settings();

    // Manual installs may retain third-party same-name legacy binaries. Only
    // the hash of a known released legacy build can authorize removal.
    prepare_root(argv[1], 740, false);
    for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
    update_cleanup_known_legacy(test_root, "0.7.0");
    for (unsigned i = 0; i < 2; i++) {
        char original[PATH_MAX]; bool exists;
        assert(path_join(original, test_root, LanywhereUpdateFiles[BridgeIds[i]]));
        assert(regular_or_missing(original, &exists) && exists);
    }

    // An unrelated file at the new destination must not be overwritten.
    prepare_root(argv[1], 701, false);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    char conflict_path[PATH_MAX];
    assert(safe_parent(test_root, BridgePaths[1], true) && path_join(conflict_path, test_root, BridgePaths[1]));
    assert(write_file(conflict_path, "unrelated", 9));
    assert(update_promote_renamed(test_root, false) == UPDATE_RENAMED_APP_CONFLICT);
    uint8_t unrelated[32]; digest("unrelated", 9, unrelated); assert(matches_file(conflict_path, 9, unrelated));

    // Both promotion entrypoint replacements can be interrupted. Recovery of a
    // committed transaction resumes safely from its immutable bridge journal.
    for (unsigned failure = 1; failure <= 2; failure++) {
        prepare_root(argv[1], 701 + failure, false);
        assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
        pid_t child = fork(); assert(child >= 0);
        if (!child) { mutation_count = 0; fault_at = failure; update_promote_renamed(test_root, false); _exit(10); }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        mutation_count = fault_at = 0;
        assert(update_recover(test_root) == UPDATE_INSTALLED);
        assert(update_promote_renamed(test_root, false) == UPDATE_OK);
        assert(update_acknowledge(test_root) == UPDATE_OK);
        assert_settings();
    }

    // Later updates are launched from the renamed entrypoint; preserve both
    // existing binaries through a failure between unlink and rename.
    for (unsigned failure = 1; failure <= 4; failure++) {
        prepare_root(argv[1], 710 + failure, false);
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
        establish_signed_ownership(test_root, "0.7.0");
        assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
        mutation_count = 0; fault_at = failure; inject_fail = true;
        assert(update_promote_renamed(test_root, true) == UPDATE_RECOVERY_REQUIRED);
        fault_at = mutation_count = 0; inject_fail = false;
        assert(update_recover(test_root) == UPDATE_INSTALLED);
        assert(update_promote_renamed(test_root, true) == UPDATE_OK);
        assert(update_acknowledge(test_root) == UPDATE_OK); assert_settings();
    }
    // Hard interruptions in the same four replacement operations must also
    // resume from the immutable bridge journal after process restart.
    for (unsigned failure = 1; failure <= 4; failure++) {
        prepare_root(argv[1], 720 + failure, false);
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
        establish_signed_ownership(test_root, "0.7.0");
        assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
        pid_t child = fork(); assert(child >= 0);
        if (!child) { mutation_count = 0; fault_at = failure; update_promote_renamed(test_root, true); _exit(10); }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        fault_at = mutation_count = 0;
        assert(update_recover(test_root) == UPDATE_INSTALLED);
        assert(update_promote_renamed(test_root, true) == UPDATE_OK);
        assert(update_acknowledge(test_root) == UPDATE_OK); assert_settings();
    }
    // A user may open the legacy entrypoint after an update even when a
    // previous renamed installation already exists. Its persisted ownership
    // record allows exactly those previously verified files to be promoted.
    prepare_root(argv[1], 725, false);
    for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
    establish_signed_ownership(test_root, "0.7.0");
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_promote_renamed(test_root, false) == UPDATE_OK);
    assert(update_acknowledge(test_root) == UPDATE_OK); assert_settings();

    // A signed manual/Tuner install B over an already-owned A must refresh
    // the owner record before the next OTA C. An invalid proof preserves A.
    prepare_root(argv[1], 1000, false);
    for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.6.9");
    establish_signed_ownership(test_root, "0.6.9");
    char manual_owner[PATH_MAX]; uint32_t old_owner_size, after_owner_size; uint8_t old_owner_hash[32], after_owner_hash[32];
    assert(path_join(manual_owner, test_root, BRIDGE_OWNER) && file_digest(manual_owner, &old_owner_size, old_owner_hash));
    pid_t unchanged = fork(); assert(unchanged >= 0);
    if (!unchanged) { owner_fault = true; fault_at = 1; mutation_count = 0; update_record_renamed_ownership(test_root, "0.6.9"); _exit(0); }
    int unchanged_status; assert(waitpid(unchanged, &unchanged_status, 0) == unchanged && WIFEXITED(unchanged_status) && WEXITSTATUS(unchanged_status) == 0);
    for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
    sign_known_entrypoints(test_root, "0.6.9");
    update_record_renamed_ownership(test_root, "0.7.0");
    assert(file_digest(manual_owner, &after_owner_size, after_owner_hash) && old_owner_size == after_owner_size && !memcmp(old_owner_hash, after_owner_hash, 32));
    sign_known_entrypoints(test_root, "0.7.0");
    update_record_renamed_ownership(test_root, "0.7.0");
    for (unsigned i = 0; i < 2; i++) { char target[PATH_MAX]; assert(path_join(target, test_root, BridgePaths[i]) && bridge_owned_matches(test_root, i, target)); }
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_promote_renamed(test_root, true) == UPDATE_OK && update_acknowledge(test_root) == UPDATE_OK);

    // Fail or lose power at each owner replacement boundary. The old owner
    // remains recoverable, and the signed B record can be retried after boot.
    for (unsigned mode = 0; mode < 2; mode++) for (unsigned point = 1; point <= 2; point++) {
        prepare_root(argv[1], 1010 + 2 * mode + point, false);
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.6.9");
        establish_signed_ownership(test_root, "0.6.9");
        assert(path_join(manual_owner, test_root, BRIDGE_OWNER) && file_digest(manual_owner, &old_owner_size, old_owner_hash));
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
        sign_known_entrypoints(test_root, "0.7.0");
        if (mode == 0) {
            owner_fault = inject_fail = true; fault_at = point; mutation_count = 0;
            update_record_renamed_ownership(test_root, "0.7.0");
            owner_fault = inject_fail = false; fault_at = 0;
            assert(file_digest(manual_owner, &after_owner_size, after_owner_hash) && old_owner_size == after_owner_size && !memcmp(old_owner_hash, after_owner_hash, 32));
        } else {
            pid_t child = fork(); assert(child >= 0);
            if (!child) { owner_fault = true; fault_at = point; mutation_count = 0; update_record_renamed_ownership(test_root, "0.7.0"); _exit(0); }
            int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        }
        update_record_renamed_ownership(test_root, "0.7.0");
        for (unsigned i = 0; i < 2; i++) { char target[PATH_MAX]; assert(path_join(target, test_root, BridgePaths[i]) && bridge_owned_matches(test_root, i, target)); }
    }
    // Inject failures at every directory commit around backup, new owner,
    // recovery and backup removal, including immediate power loss.
    for (unsigned mode = 0; mode < 2; mode++) for (unsigned point = 1; point <= 4; point++) {
        prepare_root(argv[1], 1020 + 4 * mode + point, false);
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.6.9");
        establish_signed_ownership(test_root, "0.6.9");
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
        sign_known_entrypoints(test_root, "0.7.0");
        if (mode == 0) {
            directory_fault = inject_fail = true; fault_at = point; mutation_count = 0;
            update_record_renamed_ownership(test_root, "0.7.0");
            directory_fault = inject_fail = false; fault_at = 0;
        } else {
            pid_t child = fork(); assert(child >= 0);
            if (!child) { directory_fault = true; fault_at = point; mutation_count = 0; update_record_renamed_ownership(test_root, "0.7.0"); _exit(0); }
            int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        }
        char previous[PATH_MAX]; assert(path_join(manual_owner, test_root, BRIDGE_OWNER));
        assert(path_join(previous, test_root, BRIDGE_OWNER_PREVIOUS));
        assert(bridge_valid_owner(manual_owner) || bridge_valid_owner(previous));
        update_record_renamed_ownership(test_root, "0.7.0");
        for (unsigned i = 0; i < 2; i++) { char target[PATH_MAX]; assert(path_join(target, test_root, BridgePaths[i]) && bridge_owned_matches(test_root, i, target)); }
    }

    // A signed manual-install identity must bind version and both exact files.
    // Invalid signatures, mismatched versions and substituted files never
    // authorize replacing an existing destination, even from the new path.
    for (unsigned variant = 0; variant < 4; variant++) {
        prepare_root(argv[1], 750 + variant, false);
        for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
        sign_known_entrypoints(test_root, variant == 2 ? "0.6.9" : "0.7.0");
        char signed_path[PATH_MAX], target[PATH_MAX];
        assert(path_join(signed_path, test_root, "switch/RoomCircuit/.entrypoints-signed"));
        if (variant == 0) { FILE *bad = fopen(signed_path, "r+b"); assert(bad && fseek(bad, 100, SEEK_SET) == 0); int original = fgetc(bad); assert(original != EOF && fseek(bad, 100, SEEK_SET) == 0); assert(fputc(original ^ 1, bad) != EOF); fclose(bad); }
        if (variant == 1) { assert(path_join(target, test_root, BridgePaths[1])); assert(write_file(target, "unrelated app", 13)); }
        if (variant == 3) { assert(path_join(target, test_root, BridgePaths[0])); assert(write_file(target, "unrelated overlay", 17)); }
        update_record_renamed_ownership(test_root, "0.7.0");
        char owner[PATH_MAX]; bool exists;
        assert(path_join(owner, test_root, BRIDGE_OWNER) && regular_or_missing(owner, &exists) && !exists);
        assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
        assert(update_promote_renamed(test_root, true) != UPDATE_OK);
    }

    // Even a file with the right name must have a trusted product identity.
    prepare_root(argv[1], 716, false);
    for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.6.9");
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_promote_renamed(test_root, true) == UPDATE_RENAMED_OVERLAY_CONFLICT);

    // A rollback after replacing existing renamed binaries restores their
    // byte-identical previous contents, not the stale legacy destinations.
    prepare_root(argv[1], 726, false);
    for (unsigned i = 0; i < 2; i++) create_known_renamed(test_root, BridgePaths[i], "0.7.0");
    establish_signed_ownership(test_root, "0.7.0");
    uint8_t old_renamed_hash[2][32]; uint32_t old_renamed_size[2];
    for (unsigned i = 0; i < 2; i++) {
        char target[PATH_MAX]; assert(path_join(target, test_root, BridgePaths[i]));
        assert(file_digest(target, &old_renamed_size[i], old_renamed_hash[i]));
    }
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_promote_renamed(test_root, true) == UPDATE_OK);
    Journal prior; bool prior_found;
    assert(journal_read(test_root, &prior, &prior_found) && prior_found && rollback(test_root, &prior));
    assert(update_recover(test_root) == UPDATE_RECOVERED);
    for (unsigned i = 0; i < 2; i++) {
        char target[PATH_MAX]; assert(path_join(target, test_root, BridgePaths[i]));
        assert(matches_file(target, old_renamed_size[i], old_renamed_hash[i]));
    }
    assert(update_acknowledge(test_root) == UPDATE_OK); assert_settings();

    // The old journal can disappear before legacy entrypoint cleanup. A
    // persistent hash-bound marker resumes every interrupted cleanup safely.
    for (unsigned failure = 1; failure <= 6; failure++) {
        prepare_root(argv[1], 730 + failure, false);
        assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
        assert(update_promote_renamed(test_root, false) == UPDATE_OK);
        pid_t child = fork(); assert(child >= 0);
        if (!child) { mutation_count = 0; fault_at = failure; cleanup_fault = true; update_acknowledge(test_root); _exit(10); }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        mutation_count = fault_at = 0; cleanup_fault = false;
        const UpdateResult recovered = update_recover(test_root);
        if (recovered == UPDATE_INSTALLED) {
            assert(update_promote_renamed(test_root, false) == UPDATE_OK);
            assert(update_acknowledge(test_root) == UPDATE_OK);
        } else assert(recovered == UPDATE_OK);
        update_finish_legacy_cleanup(test_root);
        for (unsigned i = 0; i < 2; i++) {
            char old[PATH_MAX]; bool exists; assert(path_join(old, test_root, LanywhereUpdateFiles[BridgeIds[i]]));
            assert(regular_or_missing(old, &exists) && !exists);
        }
        assert_settings();
    }

    // If the legacy transaction rolls back after promotion, both new targets
    // are restored to their original absence before the rollback is confirmed.
    prepare_root(argv[1], 704, false);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_promote_renamed(test_root, false) == UPDATE_OK);
    Journal bridge_test_journal; bool bridge_found;
    assert(journal_read(test_root, &bridge_test_journal, &bridge_found) && bridge_found);
    assert(rollback(test_root, &bridge_test_journal));
    assert(update_recover(test_root) == UPDATE_RECOVERED);
    for (unsigned i = 0; i < 2; i++) {
        char target[PATH_MAX]; bool exists;
        assert(path_join(target, test_root, BridgePaths[i]));
        assert(regular_or_missing(target, &exists) && !exists);
    }
    assert(update_acknowledge(test_root) == UPDATE_OK); assert_old(false); assert_settings();

    char rooted[PATH_MAX], joined[PATH_MAX];
    assert(path_join(joined, "sdmc:/", "switch/lanywhere.nro"));
    assert(!strcmp(joined, "sdmc:/switch/lanywhere.nro"));
    assert(path_join(joined, "/", "switch/lanywhere.nro"));
    assert(!strcmp(joined, "/switch/lanywhere.nro"));
    assert(!path_join(joined, "sdmc:/", "/switch/lanywhere.nro"));
    prepare_root(argv[1], 207, false);
    const int root_length = snprintf(rooted, sizeof(rooted), "%s/", test_root);
    assert(root_length > 0 && root_length < (int)sizeof(rooted));
    // Switch-style rename still rolls back with the absolute root's final slash.
    mutation_count = 0; fault_at = 7; inject_fail = true;
    assert(update_install(rooted, &manifest, download_path, NULL, NULL) == UPDATE_RECOVERED);
    inject_fail = false; fault_at = 0;
    assert(update_recover(rooted) == UPDATE_RECOVERED);
    assert_old(false); assert_settings();
    assert(update_acknowledge(rooted) == UPDATE_OK);
    assert(update_install(rooted, &manifest, download_path, assert_commit_before_complete, rooted) == UPDATE_INSTALLED);
    assert(last_progress == UPDATE_FILE_COUNT);
    assert(update_recover(rooted) == UPDATE_INSTALLED);
    assert(update_acknowledge(rooted) == UPDATE_OK && update_transaction_pending(rooted) == UPDATE_OK);
    assert_settings();

    unsigned cases = 0;
    for (unsigned missing = 0; missing < 2; missing++) {
        unsigned mutations = missing ? 22 + 14 : 22 + 22;
        for (unsigned failure = 1; failure <= mutations; failure++) {
            prepare_root(argv[1], 1 + cases, missing); cases++;
            pid_t child = fork(); assert(child >= 0);
            if (!child) { mutation_count = 0; fault_at = failure; update_install(test_root, &manifest, download_path, NULL, NULL); _exit(10); }
            int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
            mutation_count = fault_at = 0;
            assert(update_recover(test_root) == UPDATE_RECOVERED);
            assert_old(missing); assert_settings();
            assert(update_pending_version(test_root, version) == UPDATE_RECOVERED && !strcmp(version, "0.7.0"));
            assert(update_acknowledge(test_root) == UPDATE_OK);
        }
    }
    for (unsigned failure = 1; failure <= 43; failure++) {
        prepare_root(argv[1], 300 + failure, false);
        pid_t child = fork(); assert(child >= 0);
        if (!child) { fault_at = 20; mutation_count = 0; update_install(test_root, &manifest, download_path, NULL, NULL); _exit(10); }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        child = fork(); assert(child >= 0);
        if (!child) { fault_at = failure; mutation_count = 0; update_recover(test_root); _exit(10); }
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 99);
        fault_at = mutation_count = 0;
        assert(update_recover(test_root) == UPDATE_RECOVERED); assert_old(false); assert_settings();
        assert(update_acknowledge(test_root) == UPDATE_OK);
    }
    prepare_root(argv[1], 200, false); fault_at = 7; mutation_count = 0; inject_fail = true;
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_RECOVERED);
    inject_fail = false; fault_at = 0; assert_old(false); assert_settings();
    assert(update_acknowledge(test_root) == UPDATE_OK);
    prepare_root(argv[1], 201, false);
    char path[PATH_MAX]; assert(path_join(path,test_root,"lanywhere/licenses/LANywhere.txt")); assert(unlink(path) == 0);
    assert(symlink("../../config.ini", path) == 0);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_IO); assert_settings();
    prepare_root(argv[1], 202, false); FILE *f=fopen(download_path,"r+b");assert(f);assert(fputc(0,f)!=EOF);fclose(f);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INVALID); assert_old(false);
    prepare_root(argv[1], 204, false);
    /* Old files can be larger than the new bundle. This space would pass the
     * former 2*raw+1MiB heuristic while lacking room for its backup and rollback. */
    assert(path_join(path, test_root, LanywhereUpdateFiles[19]));
    uint8_t *large_old = calloc(1, 2 * 1024 * 1024); assert(large_old);
    assert(write_file(path, large_old, 2 * 1024 * 1024));
    uint8_t prior_hash[32]; uint32_t prior_size;
    assert(file_digest(path, &prior_size, prior_hash));
    fake_available = (uint64_t)manifest.raw_size * 2 + 1024 * 1024;
    mutation_count = 0;
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_SPACE);
    assert(mutation_count == 0 && matches_file(path, prior_size, prior_hash));
    char staged[PATH_MAX]; bool exists;
    assert(transaction_path(staged, test_root, "new", 0) && regular_or_missing(staged, &exists) && !exists);
    assert_settings(); free(large_old); fake_available = 0;
    prepare_root(argv[1], 205, false);
    assert(update_transaction_pending(test_root) == UPDATE_OK);
    assert(safe_parent(test_root, TXN "/new-00", true));
    assert(update_transaction_pending(test_root) == UPDATE_OK);
    assert(transaction_path(staged, test_root, "new", 0));
    assert(write_file(staged, "unfinished", 10));
    assert(update_transaction_pending(test_root) == UPDATE_RECOVERY_REQUIRED);
    uint8_t stage_hash[32]; digest("unfinished", 10, stage_hash);
    assert(matches_file(staged, 10, stage_hash)); assert_old(false); assert_settings();
    assert(unlink(staged) == 0); assert(path_join(path, test_root, TXN)); assert(rmdir(path) == 0);
    assert(symlink("../licenses", path) == 0);
    assert(update_transaction_pending(test_root) == UPDATE_RECOVERY_REQUIRED);
    struct stat link_info; assert(lstat(path, &link_info) == 0 && S_ISLNK(link_info.st_mode));
    assert(update_transaction_pending(NULL) == UPDATE_RECOVERY_REQUIRED);

    prepare_root(argv[1], 206, false);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    char recovery_path[PATH_MAX], saved_recovery[PATH_MAX];
    uint32_t recovery_size;
    uint8_t recovery_hash[32];
    assert(path_join(recovery_path, test_root, RECOVERY_APP));
    assert(path_join(saved_recovery, argv[1], "saved-recovery.nro"));
    assert(file_digest(recovery_path, &recovery_size, recovery_hash));
    assert(rename(recovery_path, saved_recovery) == 0);
    assert(update_acknowledge(test_root) == UPDATE_OK);
    // An installer can leave the Recovery NRO behind after removing its journal.
    assert(rename(saved_recovery, recovery_path) == 0);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_RECOVERY_CONFLICT);
    assert(update_transaction_pending(test_root) == UPDATE_OK);
    assert(matches_file(recovery_path, recovery_size, recovery_hash));
    assert_settings();
    assert(rename(recovery_path, saved_recovery) == 0);
    assert(update_install(test_root, &manifest, download_path, NULL, NULL) == UPDATE_INSTALLED);
    assert(update_acknowledge(test_root) == UPDATE_OK);

    prepare_root(argv[1], 203, false);
    uint8_t damaged[UPDATE_MANIFEST_SIZE]; memcpy(damaged, manifest_bytes, sizeof(damaged));
    write32(damaged + 20, 2); assert(crypto_sign_detached(damaged + 448, NULL, damaged, 448, secret_key) == 0);
    assert(update_manifest_parse(damaged, "0.7.0", &manifest) == UPDATE_UNSUPPORTED);
    memcpy(damaged,manifest_bytes,sizeof(damaged));write32(damaged + 32,UPDATE_FILE_COUNT-1);
    assert(crypto_sign_detached(damaged + 448, NULL, damaged, 448, secret_key) == 0);
    assert(update_manifest_parse(damaged,"0.7.0",&manifest)==UPDATE_INVALID);
    assert(update_manifest_parse(manifest_bytes,"0.7.0",&manifest)==UPDATE_OK);
    uint32_t raw_size=read32(manifest_bytes+28); uint8_t *raw=malloc(raw_size);assert(raw);uLongf length=raw_size;
    assert(uncompress(raw,&length,bundle,bundle_size)==Z_OK);
    Entry entries[UPDATE_FILE_COUNT];assert(bundle_entries(raw,length,"0.7.1",entries));
    assert(!bundle_entries(raw,length,"0.7.2",entries));
    write32(raw+12,1);assert(!bundle_entries(raw,length,"0.7.1",entries));write32(raw+12,0);
    write32(raw+16,UINT32_MAX);assert(!bundle_entries(raw,length,"0.7.1",entries));
    free(raw); free(bundle);
    printf("PASS signed updater: %u install and 43 rollback interruption boundaries, missing originals, signatures, versions, allowlist, symlink rejection, rollback-space preflight, read-only transaction inspection, recovery-file conflict and preserved settings\n", cases);
}
