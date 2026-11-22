/* SPDX-License-Identifier: AGPL-3.0-only */
#include "switch.h"
#include "startup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void __appInit(void);
void __appExit(void);

static const struct TestCase {
    const char *name;
    const char *calls;
    u64 emu;
    unsigned cached_hos;
    SetSysFirmwareVersion reported_hos;
    char failure;
    bool display;
    bool sd;
} cases[] = {
    {"sm-error", "M", 1, 0, {19, 0, 1}, 'M', false, false},
    {"spl-init-error", "MSm", 1, 0, {19, 0, 1}, 'S', false, false},
    {"spl-config-error", "MSQsm", 1, 0, {19, 0, 1}, 'Q', false, false},
    {"sysmmc", "MSQsm", 0, MAKEHOSVERSION(19, 0, 1), {19, 0, 1}, 0, false, false},
    {"other-emu-type", "MSQsm", 2, 0, {19, 0, 1}, 0, false, false},
    {"unsupported-cached-hos", "MSQsvvm", 1, MAKEHOSVERSION(19, 0, 0), {19, 0, 1}, 0, false, false},
    {"unsupported-reported-hos", "MSQsvTRtVvm", 1, 0, {19, 0, 0}, 0, false, false},
    {"version-read-error", "MSQsvTRtm", 1, 0, {19, 0, 1}, 'R', false, false},
    {"accepted-reported-hos", "MSQsvTRtVvAHFDCdfham", 1, 0, {19, 0, 1}, 0, true, true},
    {"accepted-cached-hos", "MSQsvvAHFDCdfham", 1, MAKEHOSVERSION(19, 0, 1), {0}, 0, true, true},
    {"sd-mount-error", "MSQsvTRtVvAHFDfham", 1, 0, {19, 0, 1}, 'D', true, false},
};

static const struct TestCase *test;
static char trace[64];
static size_t calls;
static unsigned active_hos;

static Result record(char event) {
    assert(calls < sizeof(trace) - 1);
    trace[calls++] = event;
    return event == test->failure ? 1 : 0;
}

Result smInitialize(void) { return record('M'); }
Result splInitialize(void) { return record('S'); }
Result splGetConfig(SplConfigItem item, u64 *value) {
    assert(item == 65007);
    *value = test->emu;
    return record('Q');
}
unsigned hosversionGet(void) { record('v'); return active_hos; }
void hosversionSet(unsigned value) { record('V'); active_hos = value; }
Result setsysInitialize(void) { return record('T'); }
Result setsysGetFirmwareVersion(SetSysFirmwareVersion *firmware) {
    *firmware = test->reported_hos;
    return record('R');
}
Result appletInitialize(void) { return record('A'); }
Result hidInitialize(void) { return record('H'); }
Result fsInitialize(void) { return record('F'); }
Result fsdevMountSdmc(void) { return record('D'); }
void __libnx_init_cwd(void) { record('C'); }
void fsdevUnmountAll(void) { record('d'); }
void fsExit(void) { record('f'); }
void hidExit(void) { record('h'); }
void appletExit(void) { record('a'); }
void setsysExit(void) { record('t'); }
void splExit(void) { record('s'); }
void smExit(void) { record('m'); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(cases[i].name, argv[1]) == 0) test = &cases[i];
    }
    if (!test) return 2;
    active_hos = test->cached_hos;
    __appInit();
    assert(rcEmummcDisplayReady() == test->display);
    assert(rcEmummcSdReady() == test->sd);
    __appExit();
    if (strcmp(trace, test->calls) != 0) {
        fprintf(stderr, "%s called %s, expected %s\n", test->name, trace, test->calls);
        return 1;
    }
    printf("PASS %s\n", test->name);
    return 0;
}
