#define _POSIX_C_SOURCE 200809L
#include "update_core.h"
#include "../../common/game_identity_sha256.h"
#include "../../common/update_public_key.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef UPDATE_VERIFY_PUBLIC_KEY
#define UPDATE_VERIFY_PUBLIC_KEY LanywhereUpdatePublicKey
#endif

#define TXN "lanywhere/.update"
#define JOURNAL_BYTES (88u + UPDATE_FILE_COUNT * 72u + 32u)
#define PREPARED 1u
#define COMMITTED 2u
#define ROLLED_BACK 3u
#define RECOVERY_APP "switch/lanywhere-recovery.nro"
#define BOOT_FLAG_ID 20u
#define APP_ID 19u

static atomic_flag updater_busy = ATOMIC_FLAG_INIT;
typedef struct {
    uint32_t phase;
    uint32_t existed;
    char old_version[32];
    char new_version[32];
    uint32_t old_size[UPDATE_FILE_COUNT];
    uint32_t new_size[UPDATE_FILE_COUNT];
    uint8_t old_hash[UPDATE_FILE_COUNT][32];
    uint8_t new_hash[UPDATE_FILE_COUNT][32];
} Journal;

typedef struct { const uint8_t *data; uint32_t size; uint8_t hash[32]; } Entry;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}
static void digest(const void *data, size_t size, uint8_t hash[32]) {
    GameIdentitySha256 state;
    gi_sha256_init(&state); gi_sha256_update(&state, data, size); gi_sha256_final(&state, hash);
}
static bool version_parse(const char *text, uint32_t version[3]) {
    if (!text || !*text) return false;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t value = 0;
        const char *begin = text;
        while (*text >= '0' && *text <= '9') {
            if (value > 99999) return false;
            value = value * 10 + (unsigned)(*text++ - '0');
        }
        if (text == begin || (text - begin > 1 && *begin == '0')) return false;
        version[i] = value;
        if (i < 2 && *text++ != '.') return false;
    }
    return *text == 0;
}
static bool display_text(const uint8_t *text, size_t capacity) {
    const uint8_t *end = memchr(text, 0, capacity);
    if (!end) return false;
    for (const uint8_t *p = end; p < text + capacity; p++) if (*p) return false;
    for (const uint8_t *p = text; p < end; p++) if (*p < 32 && *p != '\n') return false;
    return true;
}

#ifdef __SWITCH__
static const char *switch_random_name(void) {
    return "switch-libnx";
}

static uint32_t switch_random_u32(void) {
    uint32_t value;
    randomGet(&value, sizeof(value));
    return value;
}

static randombytes_implementation switch_random = {
    .implementation_name = switch_random_name,
    .random = switch_random_u32,
    .buf = randomGet,
};
#endif

static bool update_crypto_ready(void) {
#ifdef __SWITCH__
    // The pinned Sodium port defaults to Unix random devices. Register the
    // kernel-seeded libnx generator once, before Sodium initializes.
    static Mutex init_lock;
    static bool attempted, ready;
    mutexLock(&init_lock);
    if (!attempted) {
        ready = randombytes_set_implementation(&switch_random) == 0 && sodium_init() >= 0;
        attempted = true;
    }
    mutexUnlock(&init_lock);
    return ready;
#else
    return sodium_init() >= 0;
#endif
}

UpdateResult update_manifest_parse(const uint8_t raw[UPDATE_MANIFEST_SIZE],
                                   const char *current_version, UpdateManifest *out) {
    uint32_t current[3];
    if (!raw || !out || !version_parse(current_version, current)) return UPDATE_INVALID;
    memset(out, 0, sizeof(*out));
    if (!update_crypto_ready() || crypto_sign_verify_detached(raw + UPDATE_MANIFEST_BODY_SIZE,
            raw, UPDATE_MANIFEST_BODY_SIZE, UPDATE_VERIFY_PUBLIC_KEY) != 0) return UPDATE_SIGNATURE;
    if (memcmp(raw, "LNU1", 4) || read32(raw + 4) != UPDATE_FORMAT_VERSION ||
        read32(raw + 20) != UPDATE_FORMAT_VERSION) return UPDATE_UNSUPPORTED;
    if (!display_text(raw + 68, 160) || !display_text(raw + 228, 160)) return UPDATE_INVALID;
    for (size_t i = 388; i < UPDATE_MANIFEST_BODY_SIZE; i++) if (raw[i]) return UPDATE_INVALID;
    out->compressed_size = read32(raw + 24); out->raw_size = read32(raw + 28);
    out->file_count = read32(raw + 32);
    if (!out->compressed_size || out->compressed_size > UPDATE_MAX_COMPRESSED ||
        out->raw_size < 12 + UPDATE_FILE_COUNT * 40 || out->raw_size > UPDATE_MAX_RAW ||
        out->file_count != UPDATE_FILE_COUNT) return UPDATE_INVALID;
    int comparison = 0;
    for (unsigned i = 0; i < 3; i++) {
        out->version_numbers[i] = read32(raw + 8 + 4 * i);
        if (out->version_numbers[i] > 999999) return UPDATE_INVALID;
        if (!comparison && out->version_numbers[i] != current[i])
            comparison = out->version_numbers[i] > current[i] ? 1 : -1;
    }
    snprintf(out->version, sizeof(out->version), "%u.%u.%u", out->version_numbers[0],
             out->version_numbers[1], out->version_numbers[2]);
    snprintf(out->current_version, sizeof(out->current_version), "%s", current_version);
    memcpy(out->notes_en, raw + 68, sizeof(out->notes_en));
    memcpy(out->notes_zh, raw + 228, sizeof(out->notes_zh));
    memcpy(out->bundle_sha256, raw + 36, 32); memcpy(out->raw_manifest, raw, UPDATE_MANIFEST_SIZE);
    return comparison < 0 ? UPDATE_OLDER : comparison ? UPDATE_OK : UPDATE_NO_UPDATE;
}

static bool path_join(char path[PATH_MAX], const char *root, const char *relative) {
    int length = snprintf(path, PATH_MAX, "%s/%s", root, relative);
    return length > 0 && length < PATH_MAX;
}
static bool regular_or_missing(const char *path, bool *exists) {
    struct stat info;
    if (lstat(path, &info) == 0) { *exists = true; return S_ISREG(info.st_mode); }
    *exists = false; return errno == ENOENT;
}
static bool safe_parent(const char *root, const char *relative, bool create) {
    char path[PATH_MAX];
    if (!path_join(path, root, relative)) return false;
    struct stat info;
    if (lstat(root, &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    for (char *p = path + strlen(root) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (lstat(path, &info) != 0) {
            if (!create || errno != ENOENT || mkdir(path, 0777) != 0 || lstat(path, &info) != 0) return false;
        }
        if (!S_ISDIR(info.st_mode)) return false;
        *p = '/';
    }
    return true;
}
static bool durable_directory(const char *root) {
#ifdef __SWITCH__
    (void)root;
    return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
#else
    int descriptor = open(root, O_RDONLY);
    if (descriptor < 0) return false;
    bool ok = fsync(descriptor) == 0;
    if (close(descriptor) != 0) ok = false;
    return ok;
#endif
}
static bool file_digest(const char *path, uint32_t *size, uint8_t hash[32]) {
    bool exists;
    if (!regular_or_missing(path, &exists) || !exists) return false;
    FILE *stream = fopen(path, "rb");
    if (!stream) return false;
    uint8_t buffer[4096]; size_t count; uint64_t total = 0; GameIdentitySha256 state;
    gi_sha256_init(&state);
    while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
        total += count;
        if (total > UPDATE_MAX_RAW) { fclose(stream); return false; }
        gi_sha256_update(&state, buffer, count);
    }
    bool ok = !ferror(stream);
    if (fclose(stream) != 0) ok = false;
    if (!ok) return false;
    *size = (uint32_t)total; gi_sha256_final(&state, hash); return true;
}
static bool write_file(const char *path, const void *data, size_t size) {
    bool exists;
    if (!regular_or_missing(path, &exists)) return false;
    FILE *stream = fopen(path, "wb");
    if (!stream) return false;
    bool ok = fwrite(data, 1, size, stream) == size && fflush(stream) == 0 && fsync(fileno(stream)) == 0;
    if (fclose(stream) != 0) ok = false;
    return ok;
}
static bool copy_file(const char *source, const char *target) {
    bool exists;
    if (!regular_or_missing(source, &exists) || !exists || !regular_or_missing(target, &exists)) return false;
    FILE *input = fopen(source, "rb");
    if (!input) return false;
    FILE *output = fopen(target, "wb");
    if (!output) { fclose(input); return false; }
    uint8_t buffer[4096]; size_t count; bool ok = true;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0)
        if (fwrite(buffer, 1, count, output) != count) { ok = false; break; }
    if (ferror(input) || fflush(output) != 0 || fsync(fileno(output)) != 0) ok = false;
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    return ok;
}
static bool transaction_path(char path[PATH_MAX], const char *root, const char *kind, unsigned id) {
    char relative[100];
    snprintf(relative, sizeof(relative), TXN "/%s-%02u", kind, id);
    return path_join(path, root, relative);
}
static bool matches_file(const char *path, uint32_t expected_size, const uint8_t expected_hash[32]) {
    uint32_t size; uint8_t hash[32];
    return file_digest(path, &size, hash) && size == expected_size && !memcmp(hash, expected_hash, 32);
}
static bool journal_write(const char *root, const Journal *journal) {
    uint8_t bytes[JOURNAL_BYTES] = {0}; char temporary[PATH_MAX], path[PATH_MAX];
    memcpy(bytes, "LNUT", 4); write32(bytes + 4, 1); write32(bytes + 8, journal->phase);
    write32(bytes + 12, journal->existed); memcpy(bytes + 16, journal->old_version, 32);
    memcpy(bytes + 48, journal->new_version, 32); write32(bytes + 80, UPDATE_FILE_COUNT);
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        uint8_t *entry = bytes + 88 + i * 72;
        write32(entry, journal->old_size[i]); write32(entry + 4, journal->new_size[i]);
        memcpy(entry + 8, journal->old_hash[i], 32); memcpy(entry + 40, journal->new_hash[i], 32);
    }
    digest(bytes, sizeof(bytes) - 32, bytes + sizeof(bytes) - 32);
    bool exists;
    return path_join(temporary, root, TXN "/journal-next") && path_join(path, root, TXN "/journal") &&
        regular_or_missing(path, &exists) && !exists && write_file(temporary, bytes, sizeof(bytes)) &&
        rename(temporary, path) == 0 && durable_directory(root);
}
static bool journal_read(const char *root, Journal *journal, bool *found) {
    char path[PATH_MAX]; uint8_t bytes[JOURNAL_BYTES], hash[32];
    if (!path_join(path, root, TXN "/journal") || !regular_or_missing(path, found)) return false;
    if (!*found) return true;
    FILE *stream = fopen(path, "rb"); if (!stream) return false;
    bool ok = fread(bytes, 1, sizeof(bytes), stream) == sizeof(bytes) && fgetc(stream) == EOF && !ferror(stream);
    if (fclose(stream) != 0) ok = false;
    if (!ok || memcmp(bytes, "LNUT", 4) || read32(bytes + 4) != 1 || read32(bytes + 80) != UPDATE_FILE_COUNT ||
        read32(bytes + 84) || read32(bytes + 8) != PREPARED ||
        (read32(bytes + 12) >> UPDATE_FILE_COUNT) || !display_text(bytes + 16, 32) || !display_text(bytes + 48, 32)) return false;
    uint32_t version[3];
    if (!version_parse((const char *)bytes + 16, version) || !version_parse((const char *)bytes + 48, version)) return false;
    digest(bytes, sizeof(bytes) - 32, hash);
    if (memcmp(hash, bytes + sizeof(bytes) - 32, 32)) return false;
    memset(journal, 0, sizeof(*journal)); journal->phase = read32(bytes + 8); journal->existed = read32(bytes + 12);
    memcpy(journal->old_version, bytes + 16, 32); memcpy(journal->new_version, bytes + 48, 32);
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        const uint8_t *entry = bytes + 88 + i * 72;
        journal->old_size[i] = read32(entry); journal->new_size[i] = read32(entry + 4);
        if (journal->old_size[i] > UPDATE_MAX_RAW || journal->new_size[i] > UPDATE_MAX_RAW) return false;
        memcpy(journal->old_hash[i], entry + 8, 32); memcpy(journal->new_hash[i], entry + 40, 32);
    }
    const char *names[] = {"committed", "rolled-back"};
    for (unsigned i = 0; i < 2; i++) {
        char relative[100]; uint8_t marker[36];
        snprintf(relative, sizeof(relative), TXN "/%s", names[i]);
        if (!path_join(path, root, relative)) return false;
        bool exists;
        if (!regular_or_missing(path, &exists)) return false;
        if (!exists) continue;
        FILE *file = fopen(path, "rb");
        if (!file) return false;
        bool valid = fread(marker, 1, sizeof(marker), file) == sizeof(marker) && fgetc(file) == EOF && !ferror(file);
        if (fclose(file) != 0) valid = false;
        if (valid && !memcmp(marker, "LNCM", 4) && !memcmp(marker + 4, hash, 32))
            journal->phase = i ? ROLLED_BACK : COMMITTED;
    }
    return true;
}
static bool mark_transaction(const char *root, const char *name) {
    char path[PATH_MAX], relative[100]; uint8_t marker[36], bytes[JOURNAL_BYTES];
    if (!path_join(path, root, TXN "/journal")) return false;
    FILE *file = fopen(path, "rb"); if (!file) return false;
    bool ok = fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes) && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    memcpy(marker, "LNCM", 4); digest(bytes, sizeof(bytes) - 32, marker + 4);
    snprintf(relative, sizeof(relative), TXN "/%s", name);
    return path_join(path, root, relative) && write_file(path, marker, sizeof(marker)) && durable_directory(root);
}
static bool replace_file(const char *source, const char *target) {
    bool exists;
    if (!regular_or_missing(target, &exists)) return false;
    if (exists && unlink(target) != 0) return false;
    return rename(source, target) == 0;
}
static bool cleanup_recovery_app(const char *root) {
    char marker_path[PATH_MAX], app_path[PATH_MAX]; bool exists;
    if (!path_join(marker_path, root, TXN "/recovery-owned") || !regular_or_missing(marker_path, &exists)) return false;
    if (!exists) return true;
    uint8_t marker[40]; FILE *stream = fopen(marker_path, "rb"); if (!stream) return false;
    bool ok = fread(marker, 1, sizeof(marker), stream) == sizeof(marker) && fgetc(stream) == EOF && !ferror(stream);
    if (fclose(stream) != 0) ok = false;
    if (!path_join(app_path, root, RECOVERY_APP) || !regular_or_missing(app_path, &exists)) return false;
    if (exists && (!ok || memcmp(marker, "LNR1", 4) ||
        !matches_file(app_path, read32(marker + 4), marker + 8) || unlink(app_path) != 0)) return false;
    return unlink(marker_path) == 0;
}
static bool cleanup_transaction(const char *root) {
    char path[PATH_MAX]; bool exists;
    /* Removing the immutable journal commits cleanup. Remaining files are then disposable. */
    if (!path_join(path, root, TXN "/journal") || !regular_or_missing(path, &exists)) return false;
    if (exists && (unlink(path) != 0 || !durable_directory(root))) return false;
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        for (unsigned kind = 0; kind < 2; kind++) {
            if (!transaction_path(path, root, kind ? "old" : "new", i) || !regular_or_missing(path, &exists)) return false;
            if (exists && unlink(path) != 0) return false;
        }
    }
    const char *leaves[] = {"restore", "journal-next", "committed", "rolled-back", "recovery-next"};
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        char relative[100]; snprintf(relative, sizeof(relative), TXN "/%s", leaves[i]);
        if (!path_join(path, root, relative) || !regular_or_missing(path, &exists)) return false;
        if (exists && unlink(path) != 0) return false;
    }
    return cleanup_recovery_app(root) && durable_directory(root);
}
static bool verify_installed(const char *root, const Journal *journal, bool old) {
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        char path[PATH_MAX]; bool exists;
        if (!safe_parent(root, LanywhereUpdateFiles[i], false) || !path_join(path, root, LanywhereUpdateFiles[i]) ||
            !regular_or_missing(path, &exists)) return false;
        if (old && !(journal->existed & (1u << i))) { if (exists) return false; continue; }
        if (!exists || !matches_file(path, old ? journal->old_size[i] : journal->new_size[i],
                                    old ? journal->old_hash[i] : journal->new_hash[i])) return false;
    }
    return true;
}
static bool rollback(const char *root, Journal *journal) {
    /* Validate every backup before modifying any destination. */
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        char backup[PATH_MAX];
        if ((journal->existed & (1u << i)) && (!transaction_path(backup, root, "old", i) ||
            !matches_file(backup, journal->old_size[i], journal->old_hash[i]))) return false;
    }
    char flag_path[PATH_MAX]; bool flag_exists;
    if (!safe_parent(root, LanywhereUpdateFiles[BOOT_FLAG_ID], true) ||
        !path_join(flag_path, root, LanywhereUpdateFiles[BOOT_FLAG_ID]) ||
        !regular_or_missing(flag_path, &flag_exists) || (flag_exists && unlink(flag_path) != 0) || !durable_directory(root))
        return false;
    for (unsigned step = 0; step < UPDATE_FILE_COUNT; step++) {
        const unsigned i = step == UPDATE_FILE_COUNT - 1 ? BOOT_FLAG_ID : step >= BOOT_FLAG_ID ? step + 1 : step;
        char target[PATH_MAX], backup[PATH_MAX], temporary[PATH_MAX]; bool exists;
        if (!safe_parent(root, LanywhereUpdateFiles[i], true) || !path_join(target, root, LanywhereUpdateFiles[i]) ||
            !regular_or_missing(target, &exists)) return false;
        if (journal->existed & (1u << i)) {
            if (!transaction_path(backup, root, "old", i) || !path_join(temporary, root, TXN "/restore") ||
                !copy_file(backup, temporary) || !matches_file(temporary, journal->old_size[i], journal->old_hash[i]) ||
                !replace_file(temporary, target)) return false;
        } else if (exists && unlink(target) != 0) return false;
        if (!durable_directory(root)) return false;
    }
    journal->phase = ROLLED_BACK;
    return mark_transaction(root, "rolled-back");
}

static UpdateResult recover_locked(const char *root) {
    char path[PATH_MAX]; struct stat info;
    if (!path_join(path, root, TXN)) return UPDATE_INVALID;
    if (lstat(path, &info) != 0) return errno == ENOENT ? UPDATE_OK : UPDATE_IO;
    if (!S_ISDIR(info.st_mode) || !safe_parent(root, TXN "/journal", false)) return UPDATE_RECOVERY_REQUIRED;
    Journal journal; bool found;
    if (!journal_read(root, &journal, &found)) return UPDATE_RECOVERY_REQUIRED;
    if (!found) return cleanup_transaction(root) ? UPDATE_OK : UPDATE_RECOVERY_REQUIRED;
    if (journal.phase == PREPARED && !rollback(root, &journal)) return UPDATE_RECOVERY_REQUIRED;
    bool old = journal.phase == ROLLED_BACK;
    if (!verify_installed(root, &journal, old)) return UPDATE_RECOVERY_REQUIRED;
    return old ? UPDATE_RECOVERED : UPDATE_INSTALLED;
}
UpdateResult update_transaction_pending(const char *root) {
    char path[PATH_MAX]; struct stat info;
    if (!root || lstat(root, &info) != 0 || !S_ISDIR(info.st_mode)) return UPDATE_RECOVERY_REQUIRED;
    const char *directories[] = {"lanywhere", TXN};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        if (!path_join(path, root, directories[i])) return UPDATE_RECOVERY_REQUIRED;
        if (lstat(path, &info) != 0) return errno == ENOENT ? UPDATE_OK : UPDATE_RECOVERY_REQUIRED;
        if (!S_ISDIR(info.st_mode)) return UPDATE_RECOVERY_REQUIRED;
    }
    DIR *directory = opendir(path);
    if (!directory) return UPDATE_RECOVERY_REQUIRED;
    UpdateResult result = UPDATE_OK;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            result = UPDATE_RECOVERY_REQUIRED;
            break;
        }
    }
    if (errno) result = UPDATE_RECOVERY_REQUIRED;
    if (closedir(directory) != 0) result = UPDATE_RECOVERY_REQUIRED;
    return result;
}
UpdateResult update_recover(const char *root) {
    if (atomic_flag_test_and_set(&updater_busy)) return UPDATE_RECOVERY_REQUIRED;
    UpdateResult result = recover_locked(root);
    atomic_flag_clear(&updater_busy); return result;
}
UpdateResult update_pending_version(const char *root, char version[32]) {
    Journal journal; bool found;
    if (!version || !safe_parent(root, TXN "/journal", false) || !journal_read(root, &journal, &found) || !found)
        return UPDATE_RECOVERY_REQUIRED;
    if (journal.phase == PREPARED) return UPDATE_RECOVERY_REQUIRED;
    memcpy(version, journal.phase == COMMITTED ? journal.new_version : journal.old_version, 32);
    return journal.phase == COMMITTED ? UPDATE_INSTALLED : UPDATE_RECOVERED;
}
UpdateResult update_acknowledge(const char *root) {
    if (atomic_flag_test_and_set(&updater_busy)) return UPDATE_RECOVERY_REQUIRED;
    Journal journal; bool found; UpdateResult result = UPDATE_RECOVERY_REQUIRED;
    if (safe_parent(root, TXN "/journal", false) && journal_read(root, &journal, &found) && found &&
        journal.phase != PREPARED && verify_installed(root, &journal, journal.phase == ROLLED_BACK) && cleanup_transaction(root))
        result = UPDATE_OK;
    atomic_flag_clear(&updater_busy); return result;
}

static bool nro_control(const uint8_t *data, size_t size, uint32_t *control_offset) {
    if (size < 128 || memcmp(data + 16, "NRO0", 4)) return false;
    uint32_t asset = read32(data + 24);
    if (asset > size - 56 || memcmp(data + asset, "ASET", 4)) return false;
    const uint8_t *section = data + asset + 24;
    uint32_t offset = read32(section), length = read32(section + 8);
    if (read32(section + 4) || read32(section + 12) || length != 0x4000 ||
        offset > size - asset || length > size - asset - offset) return false;
    *control_offset = asset + offset;
    return true;
}

static bool bundle_entries(const uint8_t *raw, size_t size, const char *version, Entry entries[UPDATE_FILE_COUNT]) {
    if (size < 12 || memcmp(raw, "LNUB", 4) || read32(raw + 4) != UPDATE_FORMAT_VERSION || read32(raw + 8) != UPDATE_FILE_COUNT)
        return false;
    size_t offset = 12;
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        if (size - offset < 40 || read32(raw + offset) != i) return false;
        uint32_t length = read32(raw + offset + 4);
        if (length > size - offset - 40) return false;
        entries[i].size = length; entries[i].data = raw + offset + 40;
        memcpy(entries[i].hash, raw + offset + 8, 32);
        uint8_t hash[32]; digest(entries[i].data, length, hash);
        if (memcmp(hash, entries[i].hash, 32)) return false;
        offset += 40 + length;
    }
    /* The boot flag is always empty, and runtime files need their format headers. */
    if (offset != size || entries[20].size != 0 || entries[21].size < 16 || memcmp(entries[21].data, "PFS0", 4)) return false;
    for (unsigned i = 18; i <= 19; i++) {
        uint32_t control;
        if (!nro_control(entries[i].data, entries[i].size, &control) ||
            !memchr(entries[i].data + control + 0x3060, 0, 16) ||
            strcmp((const char *)entries[i].data + control + 0x3060, version)) return false;
    }
    return true;
}

static bool write_recovery_app(const char *root, const Entry *app) {
    uint32_t control;
    if (!nro_control(app->data, app->size, &control)) return false;
    uint8_t *copy = malloc(app->size);
    if (!copy) return false;
    memcpy(copy, app->data, app->size);
    for (unsigned language = 0; language < 16; language++) {
        char *title = (char *)copy + control + language * 0x300;
        memset(title, 0, 0x200); memcpy(title, "LANywhere Recovery", sizeof("LANywhere Recovery"));
    }
    char staged[PATH_MAX], target[PATH_MAX], marker_path[PATH_MAX]; bool exists;
    uint8_t marker[40]; memcpy(marker, "LNR1", 4); write32(marker + 4, app->size);
    digest(copy, app->size, marker + 8);
    bool ok = safe_parent(root, RECOVERY_APP, true) && path_join(staged, root, TXN "/recovery-next") &&
        path_join(target, root, RECOVERY_APP) && regular_or_missing(target, &exists) && !exists &&
        path_join(marker_path, root, TXN "/recovery-owned") && write_file(staged, copy, app->size) &&
        write_file(marker_path, marker, sizeof(marker)) && durable_directory(root) &&
        rename(staged, target) == 0 && durable_directory(root);
    free(copy); return ok;
}

static UpdateResult install_locked(const char *root, const UpdateManifest *manifest,
                                  const char *compressed_file, UpdateProgress progress, void *context) {
    UpdateManifest verified;
    if (!manifest) return UPDATE_INVALID;
    UpdateResult result = update_manifest_parse(manifest->raw_manifest, manifest->current_version, &verified);
    if (result != UPDATE_OK && result != UPDATE_NO_UPDATE) return result;
    result = recover_locked(root);
    if (result != UPDATE_OK) return result;
    char recovery_path[PATH_MAX];
    struct stat recovery_info;
    if (!path_join(recovery_path, root, RECOVERY_APP)) return UPDATE_INVALID;
    // With no pending transaction, a recovery file has no verified owner.
    if (lstat(recovery_path, &recovery_info) == 0) return UPDATE_RECOVERY_CONFLICT;
    if (errno != ENOENT) return UPDATE_IO;
    uint32_t size; uint8_t hash[32];
    if (!file_digest(compressed_file, &size, hash)) return UPDATE_IO;
    if (size != verified.compressed_size || memcmp(hash, verified.bundle_sha256, 32)) return UPDATE_INVALID;
    uint8_t *compressed = malloc(size), *raw = malloc(verified.raw_size);
    if (!compressed || !raw) { free(compressed); free(raw); return UPDATE_SPACE; }
    FILE *stream = fopen(compressed_file, "rb");
    if (!stream) { free(compressed); free(raw); return UPDATE_IO; }
    bool loaded = fread(compressed, 1, size, stream) == size && fgetc(stream) == EOF && !ferror(stream);
    if (fclose(stream) != 0) loaded = false;
    digest(compressed, loaded ? size : 0, hash);
    if (!loaded || memcmp(hash, verified.bundle_sha256, 32)) result = UPDATE_INVALID;
    else {
        z_stream inflater = {0};
        inflater.next_in = compressed; inflater.avail_in = size;
        inflater.next_out = raw; inflater.avail_out = verified.raw_size;
        if (inflateInit(&inflater) != Z_OK) result = UPDATE_INVALID;
        else {
            int status = inflate(&inflater, Z_FINISH);
            result = status == Z_STREAM_END && inflater.total_in == size && inflater.total_out == verified.raw_size ? UPDATE_OK : UPDATE_INVALID;
            inflateEnd(&inflater);
        }
    }
    free(compressed);
    Entry entries[UPDATE_FILE_COUNT];
    if (result != UPDATE_OK || !bundle_entries(raw, verified.raw_size, verified.version, entries)) {
        free(raw); return result == UPDATE_OK ? UPDATE_INVALID : result;
    }
    Journal journal = {0}; journal.phase = PREPARED;
    memcpy(journal.old_version, verified.current_version, 32); memcpy(journal.new_version, verified.version, 32);
    struct statvfs disk;
    if (statvfs(root, &disk) != 0 || !disk.f_frsize) { free(raw); return UPDATE_SPACE; }
    const uint64_t block_size = disk.f_frsize;
    /* Files coexist until commit: new stage, all old backups, a Recovery NRO,
     * and the largest old file copied during rollback before replacement. */
    uint64_t needed_blocks = (entries[APP_ID].size + block_size - 1) / block_size;
    uint32_t largest_old = 0;
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        char target[PATH_MAX]; bool exists;
        if (!safe_parent(root, LanywhereUpdateFiles[i], true) || !path_join(target, root, LanywhereUpdateFiles[i]) ||
            !regular_or_missing(target, &exists)) { free(raw); return UPDATE_IO; }
        journal.new_size[i] = entries[i].size; memcpy(journal.new_hash[i], entries[i].hash, 32);
        needed_blocks += (entries[i].size + block_size - 1) / block_size;
        if (exists) {
            journal.existed |= 1u << i;
            if (!file_digest(target, &journal.old_size[i], journal.old_hash[i])) { free(raw); return UPDATE_IO; }
            needed_blocks += (journal.old_size[i] + block_size - 1) / block_size;
            if (journal.old_size[i] > largest_old) largest_old = journal.old_size[i];
        }
    }
    const uint64_t rollback_blocks = (largest_old + block_size - 1) / block_size;
    const uint64_t metadata_blocks = (1024 * 1024 + JOURNAL_BYTES + 128 + block_size - 1) / block_size;
    needed_blocks += rollback_blocks + metadata_blocks;
    if (statvfs(root, &disk) != 0 || disk.f_frsize != block_size || needed_blocks > disk.f_bavail) {
        free(raw); return UPDATE_SPACE;
    }
    if (!safe_parent(root, TXN "/journal", true)) { free(raw); return UPDATE_IO; }
    for (unsigned i = 0; i < UPDATE_FILE_COUNT; i++) {
        char target[PATH_MAX], staged[PATH_MAX], backup[PATH_MAX];
        if (!path_join(target, root, LanywhereUpdateFiles[i]) || !transaction_path(staged, root, "new", i) ||
            !transaction_path(backup, root, "old", i)) { result = UPDATE_IO; break; }
        if (journal.existed & (1u << i)) {
            if (!copy_file(target, backup) || !matches_file(backup, journal.old_size[i], journal.old_hash[i])) {
                result = UPDATE_IO; break;
            }
        }
        if (!write_file(staged, entries[i].data, entries[i].size) || !matches_file(staged, entries[i].size, entries[i].hash)) {
            result = UPDATE_IO; break;
        }
    }
    if (result == UPDATE_OK && !write_recovery_app(root, &entries[APP_ID])) result = UPDATE_IO;
    free(raw);
    if (result != UPDATE_OK) { cleanup_transaction(root); return result; }
    /* A second free-space check preserves the rollback scratch budget even if
     * another writer consumed storage while the update was staging. */
    if (statvfs(root, &disk) != 0 || disk.f_frsize != block_size || rollback_blocks + metadata_blocks > disk.f_bavail) {
        cleanup_transaction(root); return UPDATE_SPACE;
    }
    if (!verify_installed(root, &journal, true)) { cleanup_transaction(root); return UPDATE_IO; }
    if (!durable_directory(root) || !journal_write(root, &journal)) return UPDATE_RECOVERY_REQUIRED;
    char flag_path[PATH_MAX]; bool flag_exists;
    if (!path_join(flag_path, root, LanywhereUpdateFiles[BOOT_FLAG_ID]) ||
        !regular_or_missing(flag_path, &flag_exists) || (flag_exists && unlink(flag_path) != 0) || !durable_directory(root))
        return UPDATE_RECOVERY_REQUIRED;
    for (unsigned step = 0; step < UPDATE_FILE_COUNT; step++) {
        const unsigned i = step == UPDATE_FILE_COUNT - 1 ? BOOT_FLAG_ID : step >= BOOT_FLAG_ID ? step + 1 : step;
        char staged[PATH_MAX], target[PATH_MAX]; bool exists;
        if (!safe_parent(root, LanywhereUpdateFiles[i], false) || !transaction_path(staged, root, "new", i) ||
            !path_join(target, root, LanywhereUpdateFiles[i]) || !regular_or_missing(target, &exists) ||
            !replace_file(staged, target) || !durable_directory(root) ||
            !matches_file(target, journal.new_size[i], journal.new_hash[i])) {
            return rollback(root, &journal) ? UPDATE_RECOVERED : UPDATE_RECOVERY_REQUIRED;
        }
        if (progress) progress(step + 1, UPDATE_FILE_COUNT, context);
    }
    journal.phase = COMMITTED;
    if (!mark_transaction(root, "committed")) return UPDATE_RECOVERY_REQUIRED;
    return UPDATE_INSTALLED;
}
UpdateResult update_install(const char *root, const UpdateManifest *manifest,
                            const char *compressed_file, UpdateProgress progress, void *context) {
    if (atomic_flag_test_and_set(&updater_busy)) return UPDATE_RECOVERY_REQUIRED;
    UpdateResult result = install_locked(root, manifest, compressed_file, progress, context);
    atomic_flag_clear(&updater_busy); return result;
}
