/* SPDX-License-Identifier: AGPL-3.0-only */
#include <switch.h>
#include "startup.h"

void __libnx_init_cwd(void);

static struct {
    bool sm;
    bool applet;
    bool hid;
    bool fs;
    bool sd;
} startup;

/* libnx's default initializer mounts SD before main. Guard that path as well. */
void __appInit(void) {
    if (R_FAILED(smInitialize())) return;
    startup.sm = true;
    if (R_FAILED(splInitialize())) return;
    u64 emummc = 0;
    const Result mode = splGetConfig((SplConfigItem)65007, &emummc);
    splExit();
    if (R_FAILED(mode) || emummc != 1) return;

    if (!hosversionGet()) {
        if (R_FAILED(setsysInitialize())) return;
        SetSysFirmwareVersion firmware;
        const Result version = setsysGetFirmwareVersion(&firmware);
        setsysExit();
        if (R_FAILED(version)) return;
        hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
    }
    if (hosversionGet() != MAKEHOSVERSION(19, 0, 1)) return;

    if (R_FAILED(appletInitialize())) return;
    startup.applet = true;
    if (R_FAILED(hidInitialize())) return;
    startup.hid = true;
    if (R_FAILED(fsInitialize())) return;
    startup.fs = true;
    startup.sd = R_SUCCEEDED(fsdevMountSdmc());
    if (startup.sd) __libnx_init_cwd();
}

void __appExit(void) {
    if (startup.sd) fsdevUnmountAll();
    if (startup.fs) fsExit();
    if (startup.hid) hidExit();
    if (startup.applet) appletExit();
    if (startup.sm) smExit();
}

bool rcEmummcDisplayReady(void) {
    return startup.hid;
}

bool rcEmummcSdReady(void) {
    return startup.sd;
}
