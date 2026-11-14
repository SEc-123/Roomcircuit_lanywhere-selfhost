#include "game_catalog.h"

#include <string.h>

enum { HeaderSize = 20, TitleSize = 16, AliasSize = 24 };

static uint32_t read32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read64(const unsigned char *p)
{
    return read32(p) | ((uint64_t)read32(p + 4) << 32);
}

bool game_catalog_open(GameCatalog *catalog, const void *data, size_t size)
{
    if (!catalog) return false;
    *catalog = (GameCatalog){0};
    const unsigned char *p = data;
    if (!p || size < HeaderSize || memcmp(p, "LYCAT1\0\0", 8) != 0) return false;
    const size_t titles = read32(p + 8), aliases = read32(p + 12), text = read32(p + 16);
    const size_t remaining = size - HeaderSize;
    if (!titles || titles > remaining / TitleSize || aliases > (remaining - titles * TitleSize) / AliasSize ||
        !text || text != remaining - titles * TitleSize - aliases * AliasSize)
        return false;
    const unsigned char *alias_data = p + HeaderSize + titles * TitleSize;
    *catalog = (GameCatalog){p + HeaderSize, alias_data, alias_data + aliases * AliasSize, titles, aliases, text};
    return true;
}

static const unsigned char *find_id(const unsigned char *table, size_t count, size_t stride, uint64_t id)
{
    if (!id || id == UINT64_MAX) return NULL;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const unsigned char *row = table + mid * stride;
        const uint64_t current = read64(row);
        if (current == id) return row;
        if (current < id) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static const char *title_text(const GameCatalog *catalog, uint32_t offset)
{
    if (offset >= catalog->text_size || catalog->text[offset] == 0) return NULL;
    const unsigned char *s = catalog->text + offset;
    return memchr(s, 0, catalog->text_size - offset) ? (const char *)s : NULL;
}

static CatalogMatch by_application(const GameCatalog *catalog, uint64_t id, CatalogKind kind)
{
    const unsigned char *row = find_id(catalog->titles, catalog->title_count, TitleSize, id);
    if (!row) return (CatalogMatch){0};
    const char *en = title_text(catalog, read32(row + 8)), *zh = title_text(catalog, read32(row + 12));
    return en || zh ? (CatalogMatch){id, en, zh, kind} : (CatalogMatch){0};
}

CatalogMatch game_catalog_identify(const GameCatalog *catalog, uint64_t host_app_id, uint64_t lcid)
{
    if (!catalog) return (CatalogMatch){0};
    CatalogMatch explicit_id = by_application(catalog, host_app_id, CatalogApplication);
    if (explicit_id.kind) return explicit_id;
    const unsigned char *alias = find_id(catalog->aliases, catalog->alias_count, AliasSize, lcid);
    if (alias) {
        const uint64_t app_id = read64(alias + 8);
        if (app_id) return by_application(catalog, app_id, CatalogCommunication);
        return (CatalogMatch){0, title_text(catalog, read32(alias + 16)), title_text(catalog, read32(alias + 20)), CatalogShared};
    }
    /* Equal 64-bit values are only a display clue; NACP can use a different or shared LCID. */
    return by_application(catalog, lcid, CatalogEqualId);
}

const char *game_catalog_name(const CatalogMatch *match, bool chinese)
{
    if (!match) return NULL;
    return chinese ? (match->zh ? match->zh : match->en) : (match->en ? match->en : match->zh);
}

static bool text_contains(const char *text, const char *query)
{
    if (!query || !*query) return true;
    if (!text) return false;
    for (const unsigned char *start = (const unsigned char *)text; *start; start++) {
        const unsigned char *left = start, *right = (const unsigned char *)query;
        while (*left && *right) {
            unsigned char a=*left, b=*right;
            if (a>='A' && a<='Z') a+='a'-'A';
            if (b>='A' && b<='Z') b+='a'-'A';
            if (a!=b) break;
            left++; right++;
        }
        if (!*right) return true;
    }
    return false;
}

size_t game_catalog_choices(const GameCatalog *catalog, const char *query, size_t offset,
                            CatalogGameChoice *out, size_t capacity, size_t *next)
{
    if (next) *next = SIZE_MAX;
    if (!catalog || !out || !capacity || offset > catalog->alias_count + catalog->title_count) return 0;
    size_t count = 0;
    const size_t rows = catalog->alias_count + catalog->title_count;
    for (size_t index = offset; index < rows; index++) {
        const bool alias = index < catalog->alias_count;
        const uint64_t id = alias ? read64(catalog->aliases + index * AliasSize) :
            read64(catalog->titles + (index - catalog->alias_count) * TitleSize);
        /* A catalog title is an exact-ID filter, not a claim about its unknown LCIDs. */
        if (!id || id == UINT64_MAX || (!alias && find_id(catalog->aliases, catalog->alias_count, AliasSize, id)))
            continue;
        const CatalogMatch title = game_catalog_identify(catalog, 0, id);
        if (!title.kind || (!text_contains(title.en, query) && !text_contains(title.zh, query))) continue;
        if (count == capacity) {
            if (next) *next = index;
            break;
        }
        out[count++] = (CatalogGameChoice){id, title};
    }
    return count;
}
