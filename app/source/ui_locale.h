#pragma once
#include <stdbool.h>
#include "../../common/roomcircuit_paths.h"
#include "roomcircuit_storage.h"

#define UI_LOCALE_PATH rc_storage_path_gen(roomcircuit_storage_generation(), RC_LEGACY_PATH("language.ini"), RC_V1_PATH("language.ini"), RC_CURRENT_PATH("language.ini"))

// False means English, including missing, unreadable or invalid preferences.
bool ui_locale_load(const char *path);
// The caller creates the parent directory. Never writes the relay's config.ini.
bool ui_locale_save(const char *path, bool chinese);
