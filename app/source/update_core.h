#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../../common/update_format.h"

typedef enum {
    UPDATE_OK = 0,
    UPDATE_NO_UPDATE,
    UPDATE_OLDER,
    UPDATE_UNSUPPORTED,
    UPDATE_INVALID,
    UPDATE_SIGNATURE,
    UPDATE_IO,
    UPDATE_SPACE,
    UPDATE_RECOVERED,
    UPDATE_INSTALLED,
    UPDATE_RECOVERY_REQUIRED,
    UPDATE_RECOVERY_CONFLICT,
} UpdateResult;

typedef struct {
    char version[32];
    char current_version[32];
    char notes_en[160];
    char notes_zh[160];
    uint32_t version_numbers[3];
    uint32_t compressed_size;
    uint32_t raw_size;
    uint32_t file_count;
    uint8_t bundle_sha256[32];
    uint8_t raw_manifest[UPDATE_MANIFEST_SIZE];
} UpdateManifest;

typedef void (*UpdateProgress)(unsigned completed, unsigned total, void *context);

UpdateResult update_manifest_parse(const uint8_t raw[UPDATE_MANIFEST_SIZE],
                                   const char *current_version, UpdateManifest *out);
/* root is "sdmc:" on Switch, or an isolated directory in host tests. */
UpdateResult update_install(const char *root, const UpdateManifest *manifest,
                            const char *compressed_file, UpdateProgress progress, void *context);
/* Call before starting the relay-facing UI. Recovery may require a reboot. */
UpdateResult update_recover(const char *root);
/* Read-only inspection. An absent or empty transaction directory is clear. */
UpdateResult update_transaction_pending(const char *root);

UpdateResult update_pending_version(const char *root, char version[32]);
UpdateResult update_acknowledge(const char *root);
