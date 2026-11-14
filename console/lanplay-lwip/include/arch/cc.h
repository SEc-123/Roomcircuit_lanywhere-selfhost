#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/types.h>
#include <limits.h>
#ifndef SSIZE_MAX
#define SSIZE_MAX INTPTR_MAX
#endif
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwIP assertion: %s\n", (x)); abort(); } while (0)
#define LWIP_RAND() lanplay_random()
#ifdef __cplusplus
extern "C" {
#endif
uint32_t lanplay_random(void);
#ifdef __cplusplus
}
#endif
