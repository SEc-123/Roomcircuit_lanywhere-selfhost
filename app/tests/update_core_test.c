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
static bool inject_fail;
static uint64_t fake_available;
static int injected_statvfs(const char *path, struct statvfs *info) {
    int result = statvfs(path, info);
    if (result == 0 && fake_available) { info->f_frsize = 1; info->f_bavail = fake_available; }
    return result;
}
static bool managed(const char *path) {
    size_t length = strlen(test_root);
    if (strncmp(path, test_root, length) || path[length] != '/') return false;
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++)
        if (!strcmp(path + length + 1, LanywhereUpdateFiles[i])) return true;
    return false;
}
static bool mutation(const char *path) {
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
#include "../source/update_core.c"
#undef rename
#undef unlink
#undef statvfs

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
