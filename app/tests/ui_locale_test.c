/* The same system setting drives the player's UI with no SD preference. */
#include <assert.h>
#include <stdio.h>
#include "../source/ui_locale.c"

static SetLanguage setting;
static bool chinese, spanish, init_failed, read_failed, decode_failed;
static unsigned calls, closes;

Result setInitialize(void) { return init_failed; }
void setExit(void) { closes++; }
Result setGetSystemLanguage(u64 *out) { *out = (u64)setting; return read_failed; }
Result setMakeLanguage(u64 code, SetLanguage *out) { *out = (SetLanguage)code; return decode_failed; }
void ui_set_language(bool zh, bool es) { chinese = zh; spanish = es; calls++; }

int main(void)
{
    const SetLanguage readings[] = {
        SetLanguage_ZHCN, SetLanguage_ZHTW, SetLanguage_ZHHANS, SetLanguage_ZHHANT,
        SetLanguage_ENUS, SetLanguage_JA, SetLanguage_ES
    };
    for (unsigned i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
        setting = readings[i];
        ui_locale_initialize();
        assert(chinese == (i < 4) && !spanish);
    }
    setting = SetLanguage_ZHHANS;
    init_failed = true; ui_locale_initialize(); assert(!chinese && closes == 7);
    init_failed = false; read_failed = true; ui_locale_initialize(); assert(!chinese && closes == 8);
    read_failed = false; decode_failed = true; ui_locale_initialize(); assert(!chinese && closes == 9);
    assert(calls == 10);
    puts("PASS system language: all Chinese variants, English fallback and balanced service lifetime");
}
