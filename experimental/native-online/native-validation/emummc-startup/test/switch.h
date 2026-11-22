/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef ROOMCIRCUIT_TEST_SWITCH_H
#define ROOMCIRCUIT_TEST_SWITCH_H

#include <stdint.h>

typedef uint32_t Result;
typedef uint64_t u64;
typedef uint32_t SplConfigItem;
typedef struct { unsigned major, minor, micro; } SetSysFirmwareVersion;

#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
#define MAKEHOSVERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))

Result smInitialize(void);
Result splInitialize(void);
Result splGetConfig(SplConfigItem item, u64 *value);
unsigned hosversionGet(void);
void hosversionSet(unsigned value);
Result setsysInitialize(void);
Result setsysGetFirmwareVersion(SetSysFirmwareVersion *firmware);
Result appletInitialize(void);
Result hidInitialize(void);
Result fsInitialize(void);
Result fsdevMountSdmc(void);
void fsdevUnmountAll(void);
void fsExit(void);
void hidExit(void);
void appletExit(void);
void setsysExit(void);
void splExit(void);
void smExit(void);

#endif
