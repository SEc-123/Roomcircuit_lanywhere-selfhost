#pragma once
#include <stddef.h>
#include <stdint.h>

/* The caller must verify the source BDF and replacement certificate hashes. */
int rc_store_replace_expired_root(const uint8_t *source, size_t source_size,
    const uint8_t *certificate, size_t certificate_size,
    uint8_t **result, size_t *result_size);
int rc_store_romfs(const uint8_t *store, size_t store_size,
    uint8_t **result, size_t *result_size);
