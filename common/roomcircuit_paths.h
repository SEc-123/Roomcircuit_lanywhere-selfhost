#pragma once
#include <stdbool.h>

/* Storage generation is committed by the system module before services start.
 * All components must use the same marker and must never create it themselves. */
#define RC_LEGACY_DIR "sdmc:/lanywhere"
#define RC_V1_CONFIG_DIR "sdmc:/switch/RoomCircuit"
#define RC_V1_DIR RC_V1_CONFIG_DIR "/data"
#define RC_V1_MARKER RC_V1_DIR "/.storage-v1"
#define RC_V1_PATH(leaf) RC_V1_DIR "/" leaf
#define RC_CONFIG_DIR "sdmc:/config/RoomCircuit"
#define RC_CURRENT_DIR RC_CONFIG_DIR "/data"
#define RC_STORAGE_MARKER RC_CURRENT_DIR "/.storage-v2"
#define RC_LEGACY_PATH(leaf) RC_LEGACY_DIR "/" leaf
#define RC_CURRENT_PATH(leaf) RC_CURRENT_DIR "/" leaf
#define RC_STORAGE_HEALTH_LEGACY_CHANGED (1u << 0)
#define RC_STORAGE_HEALTH_LEGACY_UNCHECKED (1u << 1)
#define RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT (1u << 2)
#define RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED (1u << 3)
#define RC_STORAGE_HEALTH_IPC 48u

static inline const char *rc_storage_path(bool current, const char *legacy_path, const char *current_path)
{
    return current ? current_path : legacy_path;
}

/* IPC46 values 0 and 1 retain their 0.9 meanings; 3 is the /config
 * generation. Unknown generations are rejected by callers before use. */
static inline const char *rc_storage_path_gen(unsigned generation, const char *legacy_path,
                                              const char *v1_path, const char *current_path)
{
    return generation == 3 ? current_path : generation == 1 ? v1_path : legacy_path;
}
