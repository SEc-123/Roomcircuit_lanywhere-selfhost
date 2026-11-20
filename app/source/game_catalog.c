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

enum { SearchQuerySize = 96, SearchTitleSize = 384 };

static uint32_t search_character(const unsigned char **cursor)
{
    const unsigned char *p = *cursor;
    uint32_t cp = *p++;
    unsigned tail = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    if (tail) cp &= 0x3f >> tail;
    while (tail && (*p & 0xc0) == 0x80) {
        cp = (cp << 6) | (*p++ & 0x3f);
        tail--;
    }
    *cursor = p;
    return tail ? 0 : cp;
}

static void normalize_search(const char *text, char *out, size_t capacity, bool keep_words)
{
    static const char latin1[] =
        "aaaaaaaceeeeiiii" "dnooooo ouuuuyts"
        "aaaaaaaceeeeiiii" "dnooooo ouuuuyty";
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p && used + 1 < capacity) {
        const unsigned char *start = p;
        uint32_t cp = search_character(&p);
        if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
        if (cp >= 0xc0 && cp <= 0xff) cp = (unsigned char)latin1[cp - 0xc0];
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
            out[used++] = (char)cp;
        } else if (cp >= 0x300 && cp <= 0x36f) {
            continue;
        } else if (cp < 0x80 || cp == 0xa0 || cp == 0xae || cp == 0xb7 || cp == 0x3000 || cp == 0x2122 ||
                   (cp >= 0x2000 && cp <= 0x206f)) {
            if (keep_words && used && out[used - 1] != ' ') out[used++] = ' ';
        } else {
            const size_t bytes = (size_t)(p - start);
            if (bytes >= capacity - used) break;
            memcpy(out + used, start, bytes);
            used += bytes;
        }
    }
    if (used && out[used - 1] == ' ') used--;
    out[used] = '\0';
}

static bool search_title(const CatalogMatch *title, const char *compact, const char *words)
{
    if (!*compact) return true;
    char en[SearchTitleSize], zh[SearchTitleSize];
    normalize_search(title->en, en, sizeof(en), false);
    normalize_search(title->zh, zh, sizeof(zh), false);
    if (strstr(en, compact) || strstr(zh, compact)) return true;
    while (*words) {
        char token[SearchQuerySize];
        const size_t length = strcspn(words, " ");
        memcpy(token, words, length);
        token[length] = '\0';
        bool matched = strstr(en, token) || strstr(zh, token);
        if (!matched && (!strcmp(token, "pokemon") || !strcmp(token, "宝可梦") || !strcmp(token, "寶可夢"))) {
            matched = strstr(en, "pokemon") || strstr(zh, "宝可梦") || strstr(zh, "寶可夢");
        }
        if (!matched) return false;
        words += length;
        if (*words) words++;
    }
    return true;
}

size_t game_catalog_choices(const GameCatalog *catalog, const char *query, size_t offset,
                            CatalogGameChoice *out, size_t capacity, size_t *next)
{
    if (next) *next = SIZE_MAX;
    if (!catalog || !out || !capacity || offset > catalog->alias_count + catalog->title_count) return 0;
    char compact[SearchQuerySize], words[SearchQuerySize];
    normalize_search(query, compact, sizeof(compact), false);
    normalize_search(query, words, sizeof(words), true);
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
        if (!title.kind || !search_title(&title, compact, words)) continue;
        if (count == capacity) {
            if (next) *next = index;
            break;
        }
        out[count++] = (CatalogGameChoice){id, title};
    }
    return count;
}
