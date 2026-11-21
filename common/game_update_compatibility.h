#ifndef ROOMCIRCUIT_GAME_UPDATE_COMPATIBILITY_H
#define ROOMCIRCUIT_GAME_UPDATE_COMPATIBILITY_H

#include <stdbool.h>
#include <stdint.h>

static inline uint32_t room_game_update_number(const char *text)
{
    if (!text) return 0;
    if (*text == 'v' || *text == 'V') ++text;
    uint32_t version = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t part = 0;
        if (*text < '0' || *text > '9') return 0;
        do {
            part = part * 10 + (uint32_t)(*text++ - '0');
            if (part > 1023) return 0;
        } while (*text >= '0' && *text <= '9');
        version = (version << 10) | part;
        if (i < 2 && *text++ != '.') return 0;
    }
    return *text ? 0 : version;
}

#define ROOM_KNOWN_UPDATE(major, minor, patch) (((major) << 20) | ((minor) << 10) | (patch))

/* These editions share a Local Wireless identity. Installed title metadata
 * alone cannot choose which edition the player will use. */
static inline uint64_t room_game_update_alternate(uint64_t app_id)
{
    switch (app_id) {
    case UINT64_C(0x0100A3D008C5C000): return UINT64_C(0x01008F6008C5E000);
    case UINT64_C(0x01008F6008C5E000): return UINT64_C(0x0100A3D008C5C000);
    default: return 0;
    }
}

/* A true result requires a documented incompatibility. The game still
 * decides admission using its local communication protocol. */
static inline bool room_game_updates_incompatible(uint64_t app_id, const char *host, const char *local)
{
    const uint32_t a = room_game_update_number(host), b = room_game_update_number(local);
    if (!a || !b || a == b) return false;
    const uint32_t newer = a > b ? a : b, older = a > b ? b : a;

    switch (app_id) {
    case UINT64_C(0x0100152000022000): /* Mario Kart 8 Deluxe */
        return newer == ROOM_KNOWN_UPDATE(4, 0, 1) ||
            (newer == ROOM_KNOWN_UPDATE(1, 4, 0) && older <= ROOM_KNOWN_UPDATE(1, 2, 1));
    case UINT64_C(0x01006A800016E000): /* Super Smash Bros. Ultimate */
    case UINT64_C(0x0100A3D008C5C000): /* Pokémon Scarlet */
    case UINT64_C(0x01008F6008C5E000): /* Pokémon Violet */
        return true;
    case UINT64_C(0x0100C2500FC20000): /* Splatoon 3 */
        return newer == ROOM_KNOWN_UPDATE(3, 1, 0) || newer == ROOM_KNOWN_UPDATE(6, 0, 0);
    case UINT64_C(0x010028600EBDA000): /* Super Mario 3D World + Bowser's Fury */
        return newer == ROOM_KNOWN_UPDATE(1, 2, 2) && older == ROOM_KNOWN_UPDATE(1, 0, 0);
    case UINT64_C(0x01007960049A0000): /* Bayonetta 2 */
        return newer == ROOM_KNOWN_UPDATE(1, 2, 0) && older == ROOM_KNOWN_UPDATE(1, 0, 0);
    default:
        return false;
    }
}

#undef ROOM_KNOWN_UPDATE
#endif
