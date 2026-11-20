// SPDX-License-Identifier: GPL-2.0-only
#include "ui_locale.h"
#include "ui_text.h"
#include "../../common/system_language.h"

void ui_locale_initialize(void)
{
    ui_set_language(roomcircuit_system_chinese(), false);
}
