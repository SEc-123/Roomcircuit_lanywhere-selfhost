#pragma once
#include <stdbool.h>
#include <switch.h>

/* Returns true when recovery requires exiting and restarting the console. */
bool update_ui_initialize(const char *running_path);
void update_ui_shutdown(void);
bool update_ui_available(void);
bool update_ui_changed(void);
const char *update_ui_app_version(void);
const char *update_ui_button_label(bool available);
/* Modal returns true when the caller must leave the app. */
bool update_ui_open(PadState *pad, bool (*can_install)(void *), void *context);
