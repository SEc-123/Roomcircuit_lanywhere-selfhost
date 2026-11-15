#pragma once
#include "../../../common/game_identity_text.h"

namespace ams::mitm::lanywhere {

    inline bool CopyDeviceNickname(const char *source, size_t source_size, char *out, size_t capacity) {
        if (!out || !capacity) return false;
        out[0] = '\0';
        if (!source) return false;
        const auto *end = static_cast<const char *>(memchr(source, '\0', source_size));
        if (!end) return false;
        char valid[GAME_IDENTITY_TITLE_CAP];
        if (!game_identity_safe_title(reinterpret_cast<const uint8_t *>(source), end - source, valid, sizeof(valid))) return false;

        // The relay accepts names of 2–20 codepoints with 31 bytes on the wire.
        size_t written = 0;
        unsigned characters = 0;
        for (const auto *p = reinterpret_cast<const uint8_t *>(valid); *p && characters < 20;) {
            const uint8_t first = *p;
            if (first < 0x80) {
                const bool letter_or_digit = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                    (first >= '0' && first <= '9');
                const bool separator = first == '.' || first == '_' || first == '-';
                const bool space = first == ' ' && written && out[written - 1] != ' ';
                if (!letter_or_digit && !separator && !space) {
                    ++p;
                    continue;
                }
            }
            // Complete, strictly valid UTF-8 has already been checked above.
            const size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            if (width > capacity - 1 - written) break;
            memcpy(out + written, p, width);
            written += width;
            p += width;
            ++characters;
        }
        if (written && out[written - 1] == ' ') {
            --written;
            --characters;
        }
        if (characters < 2) {
            out[0] = '\0';
            return false;
        }
        out[written] = '\0';
        return true;
    }

}
