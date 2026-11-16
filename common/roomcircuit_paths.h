#pragma once
#include <stdbool.h>

/* Storage generation is committed by the system module before services start.
 * All components must use the same marker and must never create it themselves. */
#define RC_LEGACY_DIR "sdmc:/lanywhere"
#define RC_CONFIG_DIR "sdmc:/switch/RoomCircuit"
#define RC_CURRENT_DIR RC_CONFIG_DIR "/data"
#define RC_STORAGE_MARKER RC_CURRENT_DIR "/.storage-v1"
#define RC_LEGACY_PATH(leaf) RC_LEGACY_DIR "/" leaf
#define RC_CURRENT_PATH(leaf) RC_CURRENT_DIR "/" leaf

static inline const char *rc_storage_path(bool current, const char *legacy_path, const char *current_path)
{
    return current ? current_path : legacy_path;
}
