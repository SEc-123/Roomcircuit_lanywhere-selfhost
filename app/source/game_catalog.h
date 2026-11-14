#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const unsigned char *titles, *aliases, *text;
    size_t title_count, alias_count, text_size;
} GameCatalog;

typedef enum {
    CatalogNone, CatalogApplication, CatalogCommunication, CatalogShared, CatalogEqualId,
} CatalogKind;

typedef struct {
    uint64_t app_id;
    const char *en, *zh;
    CatalogKind kind;
} CatalogMatch;

bool game_catalog_open(GameCatalog *catalog, const void *data, size_t size);
CatalogMatch game_catalog_identify(const GameCatalog *catalog, uint64_t host_app_id, uint64_t lcid);
const char *game_catalog_name(const CatalogMatch *match, bool chinese);

typedef struct {
    uint64_t game_id;
    CatalogMatch title;
} CatalogGameChoice;
/* Known LCIDs followed by catalog title IDs; each choice filters the exact raw ID.
 * Offset/next are opaque cursors. Search matches a substring in either language. */
size_t game_catalog_choices(const GameCatalog *catalog, const char *query, size_t offset,
                            CatalogGameChoice *out, size_t capacity, size_t *next);
