#ifndef ROOMCIRCUIT_SYSTEM_LANGUAGE_H
#define ROOMCIRCUIT_SYSTEM_LANGUAGE_H

#include <switch.h>

static inline bool roomcircuit_system_chinese(void)
{
    if (R_FAILED(setInitialize())) return false;
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    const bool valid = R_SUCCEEDED(setGetSystemLanguage(&code)) &&
        R_SUCCEEDED(setMakeLanguage(code, &language));
    setExit();
    return valid && (language == SetLanguage_ZHCN || language == SetLanguage_ZHTW ||
                     language == SetLanguage_ZHHANS || language == SetLanguage_ZHHANT);
}

#endif
